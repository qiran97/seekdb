// B+tree implementation used as the clustered index of every table.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "storage.h"

namespace mvdb {

namespace {

constexpr uint32_t kLeafSlotCountOffset = 16;
constexpr uint32_t kLeafHeapTopOffset = 20;
constexpr uint32_t kInternalSlotCountOffset = 16;
constexpr uint32_t kInternalRightmostOffset = 20;
constexpr uint32_t kOverflowUsedOffset = 16;

inline uint16_t load_u16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
inline void store_u16(uint8_t* p, uint16_t v) { memcpy(p, &v, sizeof(v)); }

inline uint64_t leaf_slot_key(const uint8_t* page, uint32_t index) {
  return load_u64(page + kLeafHeaderSize + index * kLeafSlotSize);
}
inline uint32_t leaf_slot_offset(const uint8_t* page, uint32_t index) {
  return load_u32(page + kLeafHeaderSize + index * kLeafSlotSize + 8);
}
inline uint32_t leaf_slot_length(const uint8_t* page, uint32_t index) {
  return load_u32(page + kLeafHeaderSize + index * kLeafSlotSize + 12);
}
inline uint32_t leaf_slot_flags(const uint8_t* page, uint32_t index) {
  return load_u32(page + kLeafHeaderSize + index * kLeafSlotSize + 16);
}
inline void leaf_set_slot(uint8_t* page, uint32_t index, uint64_t key,
                          uint32_t offset, uint32_t length, uint32_t flags) {
  uint8_t* slot = page + kLeafHeaderSize + index * kLeafSlotSize;
  store_u64(slot, key);
  store_u32(slot + 8, offset);
  store_u32(slot + 12, length);
  store_u32(slot + 16, flags);
}

inline uint32_t leaf_slot_count(const uint8_t* page) {
  return load_u16(page + kLeafSlotCountOffset);
}
inline void leaf_set_slot_count(uint8_t* page, uint32_t count) {
  store_u16(page + kLeafSlotCountOffset, static_cast<uint16_t>(count));
}
inline uint32_t leaf_heap_top(const uint8_t* page) {
  return load_u32(page + kLeafHeapTopOffset);
}
inline void leaf_set_heap_top(uint8_t* page, uint32_t value) {
  store_u32(page + kLeafHeapTopOffset, value);
}

inline uint32_t internal_slot_count(const uint8_t* page) {
  return load_u16(page + kInternalSlotCountOffset);
}
inline void internal_set_slot_count(uint8_t* page, uint32_t count) {
  store_u16(page + kInternalSlotCountOffset, static_cast<uint16_t>(count));
}
inline uint32_t internal_rightmost(const uint8_t* page) {
  return load_u32(page + kInternalRightmostOffset);
}
inline void internal_set_rightmost(uint8_t* page, uint32_t child) {
  store_u32(page + kInternalRightmostOffset, child);
}
inline uint64_t internal_slot_key(const uint8_t* page, uint32_t index) {
  return load_u64(page + kInternalHeaderSize + index * kInternalSlotSize);
}
inline uint32_t internal_slot_child(const uint8_t* page, uint32_t index) {
  return load_u32(page + kInternalHeaderSize + index * kInternalSlotSize + 8);
}
inline void internal_set_slot(uint8_t* page, uint32_t index, uint64_t key,
                               uint32_t child) {
  uint8_t* slot = page + kInternalHeaderSize + index * kInternalSlotSize;
  store_u64(slot, key);
  store_u32(slot + 8, child);
  store_u32(slot + 12, 0);
}

// Largest payload that still fits into an empty leaf page.
constexpr uint32_t kMaxInlinePayload = kPageSize - kLeafHeaderSize - kLeafSlotSize;

// Appends a slot (and its inline payload) to a leaf page.
void leaf_append_slot(uint8_t* page, uint64_t key, const uint8_t* payload,
                       uint32_t length) {
  uint32_t count = leaf_slot_count(page);
  uint32_t heap_top = leaf_heap_top(page);
  if (heap_top == 0) heap_top = kPageSize;
  uint32_t offset = heap_top - length;
  if (length > 0) memcpy(page + offset, payload, length);
  leaf_set_slot(page, count, key, offset, length, 0);
  leaf_set_slot_count(page, count + 1);
  leaf_set_heap_top(page, offset);
}

// Rebuilds the payload area of a leaf page, dropping the space of deleted
// slots.
//
// The slot array is rebuilt by default: the deleted slots are removed and the
// remaining slots move to the front of the array.  That is not allowed while a
// scan runs over the page, the scan would see the rows move to another slot
// and would visit some of them twice while skipping others.  With
// `keep_slot_layout` the array keeps its layout and only the payloads move,
// the space of the deleted rows is reclaimed nevertheless.
void leaf_compact(uint8_t* page, bool keep_slot_layout) {
  uint32_t count = leaf_slot_count(page);
  struct Entry {
    uint32_t slot;
    uint64_t key;
    uint32_t length;
    uint32_t flags;
    uint32_t old_offset;
    uint32_t new_offset;
  };
  std::vector<Entry> entries;
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t flags = leaf_slot_flags(page, i);
    if (flags & kSlotDeleted) continue;
    Entry entry;
    entry.slot = i;
    entry.key = leaf_slot_key(page, i);
    entry.length = leaf_slot_length(page, i);
    entry.flags = flags;
    entry.old_offset = leaf_slot_offset(page, i);
    // An overflow row keeps the id of its chain, the payload does not live in
    // the page.
    entry.new_offset = entry.old_offset;
    entries.push_back(entry);
  }
  // The payloads are moved through a buffer: the payloads are not laid out in
  // the order of the slots, moving them in place would overwrite a payload
  // that has not been moved yet.
  uint32_t heap_top = kPageSize;
  for (Entry& entry : entries) {
    if (entry.flags & kSlotOverflow) continue;  // the payload lives elsewhere
    heap_top -= entry.length;
    entry.new_offset = heap_top;
  }
  // The new offsets descend in the order of the slots, the buffer is filled
  // in the same order so that the copy lands on the right offsets.
  std::vector<uint8_t> buffer;
  buffer.reserve(kPageSize);
  for (size_t i = entries.size(); i-- > 0;) {
    Entry& entry = entries[i];
    if (entry.flags & kSlotOverflow) continue;
    buffer.insert(buffer.end(), page + entry.old_offset,
                  page + entry.old_offset + entry.length);
  }
  if (!buffer.empty()) {
    memcpy(page + heap_top, buffer.data(), buffer.size());
  }
  if (keep_slot_layout) {
    // The slots keep their position, only their payload moves.  The deleted
    // slots lose their payload, they are never read again.
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t flags = leaf_slot_flags(page, i);
      if (flags & kSlotDeleted) {
        leaf_set_slot(page, i, leaf_slot_key(page, i), 0, 0, flags);
      }
    }
    for (const Entry& entry : entries) {
      leaf_set_slot(page, entry.slot, entry.key, entry.new_offset, entry.length,
                    entry.flags);
    }
  } else {
    for (size_t i = 0; i < entries.size(); ++i) {
      leaf_set_slot(page, static_cast<uint32_t>(i), entries[i].key,
                    entries[i].new_offset, entries[i].length, entries[i].flags);
    }
    leaf_set_slot_count(page, static_cast<uint32_t>(entries.size()));
  }
  leaf_set_heap_top(page, heap_top);
}

// Checks the invariants of a leaf page.  The check is only executed when
// MINIVECTORDB_CHECK_PAGES is set, it is used by the differential test to
// find the statement that corrupts a page.
bool check_leaf_page(const uint8_t* page, uint64_t page_id, std::string* error) {
  uint32_t count = leaf_slot_count(page);
  uint32_t heap_top = leaf_heap_top(page);
  uint32_t slots_end = kLeafHeaderSize + count * kLeafSlotSize;
  if (slots_end > kPageSize || heap_top > kPageSize || heap_top < slots_end) {
    *error = "page " + std::to_string(page_id) + ": bad header (slots_end " +
             std::to_string(slots_end) + ", heap_top " + std::to_string(heap_top) + ")";
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t key = leaf_slot_key(page, i);
    uint32_t offset = leaf_slot_offset(page, i);
    uint32_t length = leaf_slot_length(page, i);
    uint32_t flags = leaf_slot_flags(page, i);
    if (i > 0 && leaf_slot_key(page, i - 1) > key) {
      *error = "page " + std::to_string(page_id) + ": slots are not sorted (" +
               std::to_string(leaf_slot_key(page, i - 1)) + " > " + std::to_string(key) + ")";
      return false;
    }
    if ((flags & (kSlotOverflow | kSlotDeleted)) == 0) {
      if (length >= 8) {
        // The payload starts with the number of values, it can only be a
        // small number.
        uint32_t values = load_u32(page + offset);
        if (values > 64) {
          *error = "page " + std::to_string(page_id) + ": slot " + std::to_string(i) +
                   " points at a payload with " + std::to_string(values) + " values";
          return false;
        }
      }
      if (offset < heap_top || offset + length > kPageSize) {
        *error = "page " + std::to_string(page_id) + ": slot " + std::to_string(i) +
                 " is out of the page area (offset " + std::to_string(offset) +
                 ", length " + std::to_string(length) + ", heap_top " +
                 std::to_string(heap_top) + ")";
        return false;
      }
    }
  }
  // The payloads must not overlap each other.
  std::vector<std::pair<uint32_t, uint32_t>> areas;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t flags = leaf_slot_flags(page, i);
    if (flags & (kSlotOverflow | kSlotDeleted)) continue;
    areas.emplace_back(leaf_slot_offset(page, i), leaf_slot_length(page, i));
  }
  std::sort(areas.begin(), areas.end());
  for (size_t i = 1; i < areas.size(); ++i) {
    if (areas[i].first < areas[i - 1].first + areas[i - 1].second) {
      *error = "page " + std::to_string(page_id) + ": the payloads overlap";
      return false;
    }
  }
  return true;
}

// Inserts a slot into the sorted slot array of a leaf page.
void leaf_insert_slot(uint8_t* page, uint64_t key, uint32_t offset,
                       uint32_t length, uint32_t flags) {
  uint32_t count = leaf_slot_count(page);
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(count);
  while (low < high) {
    int64_t mid = (low + high) / 2;
    if (leaf_slot_key(page, static_cast<uint32_t>(mid)) < key) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  uint32_t position = static_cast<uint32_t>(low);
  for (uint32_t i = count; i > position; --i) {
    uint8_t* source = page + kLeafHeaderSize + (i - 1) * kLeafSlotSize;
    uint8_t* target = page + kLeafHeaderSize + i * kLeafSlotSize;
    memmove(target, source, kLeafSlotSize);
  }
  leaf_set_slot(page, position, key, offset, length, flags);
  leaf_set_slot_count(page, count + 1);
}

// Writes a payload into an overflow chain and returns the first page id.
//
// The pages of the chain are written through the buffer pool.  A page that is
// written behind the back of the pool would be overwritten by the pool when
// it writes its frame back to the data file.
Status write_overflow_chain(Storage* storage, const std::string& payload,
                             uint64_t* first_page) {
  uint64_t chain = 0;
  uint64_t previous = 0;
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t overflow_page_id = 0;
    Status status = storage->allocate_page(PageType::kOverflow, &overflow_page_id);
    if (!status.ok()) return status;
    uint32_t chunk = static_cast<uint32_t>(
        std::min<size_t>(payload.size() - offset, kPageSize - kOverflowHeaderSize));
    uint8_t* overflow_page = nullptr;
    status = storage->fetch_page(overflow_page_id, &overflow_page);
    if (!status.ok()) return status;
    memcpy(overflow_page + kOverflowHeaderSize, payload.data() + offset, chunk);
    store_u32(overflow_page + kOverflowUsedOffset, chunk);
    store_u32(overflow_page + 8, 0);
    storage->mark_dirty(overflow_page_id);
    storage->unpin_page(overflow_page_id);
    if (previous == 0) {
      chain = overflow_page_id;
    } else {
      // The chain runs forwards, the previous page points to this one.
      uint8_t* previous_page = nullptr;
      status = storage->fetch_page(previous, &previous_page);
      if (!status.ok()) return status;
      store_u32(previous_page + 8, static_cast<uint32_t>(overflow_page_id));
      storage->mark_dirty(previous);
      storage->unpin_page(previous);
    }
    previous = overflow_page_id;
    offset += chunk;
  }
  *first_page = chain;
  return Status::Ok();
}

bool page_checks_enabled() {
  static const bool enabled = getenv("MINIVECTORDB_CHECK_PAGES") != nullptr;
  return enabled;
}

void check_page(uint64_t page_id, const uint8_t* page) {
  if (!page_checks_enabled()) return;
  std::string error;
  if (!check_leaf_page(page, page_id, &error)) {
    fprintf(stderr, "[page check] %s\n", error.c_str());
    fflush(stderr);
    abort();
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// BTree
// ---------------------------------------------------------------------------

Status BTree::find_leaf(uint64_t key, std::vector<uint64_t>* path,
                        uint64_t* leaf) {
  path->clear();
  uint64_t page_id = root_page_;
  if (page_id == 0) {
    *leaf = 0;
    return Status::Error(ErrorCode::kInternalError, "tree has no root page");
  }
  while (true) {
    uint8_t* page = nullptr;
    Status status = storage_->fetch_page(page_id, &page);
    if (!status.ok()) return status;
    uint32_t type = load_u32(page);
    if (type == PageType::kLeaf) {
      storage_->unpin_page(page_id);
      *leaf = page_id;
      return Status::Ok();
    }
    if (type != PageType::kInternal) {
      // A page of an unknown type would send the descent into a loop.
      storage_->unpin_page(page_id);
      return Status::Error(ErrorCode::kStorageCorrupt,
                           "the tree points at a page of an unknown type");
    }
    // Only the internal pages are recorded, the last entry of the path is
    // the parent of the leaf that is returned.
    path->push_back(page_id);
    uint32_t count = internal_slot_count(page);
    uint32_t child = internal_rightmost(page);
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t separator = internal_slot_key(page, i);
      if (separator > key) {
        child = internal_slot_child(page, i);
        break;
      }
    }
    storage_->unpin_page(page_id);
    page_id = child;
  }
}

Status BTree::read_payload(const uint8_t* page, uint32_t slot_index,
                            std::string* payload) {
  uint32_t flags = leaf_slot_flags(page, slot_index);
  uint32_t length = leaf_slot_length(page, slot_index);
  uint32_t offset = leaf_slot_offset(page, slot_index);
  payload->clear();
  if (length == 0) return Status::Ok();
  if (flags & kSlotOverflow) {
    uint64_t next = offset;
    uint32_t done = 0;
    // The buffer of the row has to exist before it is filled, the string
    // cannot grow while it is written.
    payload->resize(length);
    while (done < length && next != 0) {
      uint8_t* overflow_page = nullptr;
      Status status = storage_->fetch_page(next, &overflow_page);
      if (!status.ok()) return status;
      uint32_t used = load_u32(overflow_page + kOverflowUsedOffset);
      uint32_t chunk = std::min<uint32_t>(used, length - done);
      if (chunk > 0) memcpy(&(*payload)[done], overflow_page + kOverflowHeaderSize, chunk);
      done += chunk;
      uint64_t following = load_u32(overflow_page + 8);
      storage_->unpin_page(next);
      next = following;
    }
    if (done != length) {
      return Status::Error(ErrorCode::kStorageCorrupt,
                           "truncated overflow chain for row");
    }
    return Status::Ok();
  }
  payload->append(reinterpret_cast<const char*>(page + offset), length);
  return Status::Ok();
}
// Returns the index of the slot with the given key, -1 when the page does
// not contain the key.
int BTree::find_slot(const uint8_t* page, uint64_t key) {
  uint32_t count = leaf_slot_count(page);
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(count) - 1;
  while (low <= high) {
    int64_t mid = (low + high) / 2;
    uint64_t mid_key = leaf_slot_key(page, static_cast<uint32_t>(mid));
    if (mid_key == key) return static_cast<int>(mid);
    if (mid_key < key) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return -1;
}

Status BTree::write_payload(uint64_t leaf, uint32_t slot_index,
                            const std::string& payload) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t flags = leaf_slot_flags(page, slot_index);
  uint32_t old_length = leaf_slot_length(page, slot_index);
  uint64_t key = leaf_slot_key(page, slot_index);
  uint32_t length = static_cast<uint32_t>(payload.size());

  status = storage_->note_modification(leaf, page);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }

  if (flags & kSlotOverflow) {
    uint64_t next = leaf_slot_offset(page, slot_index);
    leaf_set_slot(page, slot_index, key, 0, 0, 0);
    flags = 0;
    old_length = 0;
    while (next != 0) {
      uint8_t* overflow_page = nullptr;
      Status read_status = storage_->fetch_page(next, &overflow_page);
      if (!read_status.ok()) break;
      uint64_t following = load_u32(overflow_page + 8);
      storage_->unpin_page(next);
      storage_->free_page(next);
      next = following;
    }
  }

  if (length <= old_length && length > 0) {
    uint32_t offset = leaf_slot_offset(page, slot_index);
    if (offset != 0) {
      memcpy(page + offset, payload.data(), length);
      leaf_set_slot(page, slot_index, key, offset, length, 0);
      storage_->mark_dirty(leaf);
      check_page(leaf, page);
      storage_->unpin_page(leaf);
      return Status::Ok();
    }
  }

  uint32_t count = leaf_slot_count(page);
  uint32_t slots_end = kLeafHeaderSize + count * kLeafSlotSize;
  uint32_t heap_top = leaf_heap_top(page);
  if (heap_top == 0) heap_top = kPageSize;
  uint32_t free_space = heap_top - slots_end;
  if (free_space < length) {
    // The compaction rebuilds the slot array, the index of the slot has to
    // be looked up again.  A scan that runs over the page keeps the layout of
    // the slot array, the index does not change then.
    leaf_compact(page, scan_depth_ > 0);
    int position = find_slot(page, key);
    if (position < 0) {
      storage_->unpin_page(leaf);
      return Status::Error(ErrorCode::kStorageCorrupt,
                           "the row disappeared from its leaf page");
    }
    slot_index = static_cast<uint32_t>(position);
    count = leaf_slot_count(page);
    slots_end = kLeafHeaderSize + count * kLeafSlotSize;
    heap_top = leaf_heap_top(page);
    free_space = heap_top - slots_end;
  }
  if (free_space >= length && length > 0) {
    uint32_t offset = heap_top - length;
    memcpy(page + offset, payload.data(), length);
    leaf_set_slot(page, slot_index, key, offset, length, 0);
    leaf_set_heap_top(page, offset);
    storage_->mark_dirty(leaf);
    check_page(leaf, page);
    storage_->unpin_page(leaf);
    return Status::Ok();
  }
  if (length == 0) {
    leaf_set_slot(page, slot_index, key, 0, 0, 0);
    storage_->mark_dirty(leaf);
    storage_->unpin_page(leaf);
    return Status::Ok();
  }

  uint64_t chain = 0;
  status = write_overflow_chain(storage_, payload, &chain);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }
  leaf_set_slot(page, slot_index, key, static_cast<uint32_t>(chain), length,
                kSlotOverflow);
  storage_->mark_dirty(leaf);
  storage_->unpin_page(leaf);
  return Status::Ok();
}

Status BTree::leaf_insert(uint64_t leaf, uint64_t key, const std::string& payload,
                          bool replace, bool allow_overflow) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t count = leaf_slot_count(page);
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(count) - 1;
  int64_t found = -1;
  while (low <= high) {
    int64_t mid = (low + high) / 2;
    uint64_t mid_key = leaf_slot_key(page, static_cast<uint32_t>(mid));
    if (mid_key == key) {
      found = mid;
      break;
    }
    if (mid_key < key) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  if (found >= 0) {
    uint32_t flags = leaf_slot_flags(page, static_cast<uint32_t>(found));
    if (!(flags & kSlotDeleted)) {
      storage_->unpin_page(leaf);
      if (!replace) {
        return Status::Error(ErrorCode::kDuplicateKey, "duplicate primary key value");
      }
      return write_payload(leaf, static_cast<uint32_t>(found), payload);
    }
    status = storage_->note_modification(leaf, page);
    if (!status.ok()) {
      storage_->unpin_page(leaf);
      return status;
    }
    leaf_set_slot(page, static_cast<uint32_t>(found), key, 0, 0, 0);
    storage_->mark_dirty(leaf);
    storage_->unpin_page(leaf);
    return write_payload(leaf, static_cast<uint32_t>(found), payload);
  }

  uint32_t length = static_cast<uint32_t>(payload.size());
  if (length > kMaxInlinePayload) {
    status = storage_->note_modification(leaf, page);
    if (!status.ok()) {
      storage_->unpin_page(leaf);
      return status;
    }
    uint32_t slots_end = kLeafHeaderSize + count * kLeafSlotSize;
    uint32_t heap_top = leaf_heap_top(page);
    if (heap_top == 0) heap_top = kPageSize;
    if (heap_top - slots_end < kLeafSlotSize) {
      storage_->unpin_page(leaf);
      return Status::Error(ErrorCode::kInternalError,
                           "leaf page without room for a slot");
    }
    uint64_t chain = 0;
    status = write_overflow_chain(storage_, payload, &chain);
    if (!status.ok()) {
      storage_->unpin_page(leaf);
      return status;
    }
    leaf_insert_slot(page, key, static_cast<uint32_t>(chain), length,
                     kSlotOverflow);
    storage_->mark_dirty(leaf);
    storage_->unpin_page(leaf);
    return Status::Ok();
  }

  uint32_t slots_end = kLeafHeaderSize + count * kLeafSlotSize;
  uint32_t heap_top = leaf_heap_top(page);
  if (heap_top == 0) heap_top = kPageSize;
  uint32_t needed = kLeafSlotSize + length;
  if (heap_top - slots_end < needed) {
    leaf_compact(page, scan_depth_ > 0);
    count = leaf_slot_count(page);
    slots_end = kLeafHeaderSize + count * kLeafSlotSize;
    heap_top = leaf_heap_top(page);
    if (heap_top - slots_end < needed) {
      if (!allow_overflow) {
        storage_->unpin_page(leaf);
        return Status::Error(ErrorCode::kStorageCorrupt, "leaf page is full");
      }
      // The row does not fit into the page, it is stored in an overflow chain.
      uint64_t chain = 0;
      status = write_overflow_chain(storage_, payload, &chain);
      if (!status.ok()) {
        storage_->unpin_page(leaf);
        return status;
      }
      status = storage_->note_modification(leaf, page);
      if (!status.ok()) {
        storage_->unpin_page(leaf);
        return status;
      }
      leaf_insert_slot(page, key, static_cast<uint32_t>(chain), length,
                       kSlotOverflow);
      storage_->mark_dirty(leaf);
      storage_->unpin_page(leaf);
      return Status::Ok();
    }
  }
  status = storage_->note_modification(leaf, page);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }
  // The slots of a leaf page are sorted by their key, the lookups binary
  // search this array.
  uint32_t offset = heap_top - length;
  memcpy(page + offset, payload.data(), length);
  leaf_insert_slot(page, key, offset, length, 0);
  leaf_set_heap_top(page, offset);
  storage_->mark_dirty(leaf);
  check_page(leaf, page);
  storage_->unpin_page(leaf);
  return Status::Ok();
}

Status BTree::split_leaf(uint64_t leaf, uint64_t* right_leaf, uint64_t* separator) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t count = leaf_slot_count(page);
  // The live rows decide where the page is split, a deleted row stays deleted
  // and must not be copied to the new page.
  std::vector<uint32_t> live;
  live.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (leaf_slot_flags(page, i) & kSlotDeleted) continue;
    live.push_back(i);
  }
  if (live.size() < 2) {
    storage_->unpin_page(leaf);
    return Status::Error(ErrorCode::kInternalError, "cannot split a single slot leaf");
  }
  size_t total = 0;
  for (uint32_t index : live) {
    total += leaf_slot_length(page, index) + kLeafSlotSize;
  }
  size_t half = total / 2;
  size_t accumulated = 0;
  size_t split = live.size() / 2;
  for (size_t index = 0; index < live.size(); ++index) {
    accumulated += leaf_slot_length(page, live[index]) + kLeafSlotSize;
    if (accumulated >= half) {
      split = index + 1;
      break;
    }
  }
  if (split >= live.size()) split = live.size() - 1;
  if (split == 0) split = 1;
  uint32_t mid = live[split];

  status = storage_->note_modification(leaf, page);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }

  uint64_t right = 0;
  status = storage_->allocate_page(PageType::kLeaf, &right);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }
  uint8_t* right_page = nullptr;
  status = storage_->fetch_page(right, &right_page);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }
  memset(right_page, 0, kPageSize);
  store_u32(right_page + 0, PageType::kLeaf);
  store_u32(right_page + 4, static_cast<uint32_t>(right));
  leaf_set_heap_top(right_page, kPageSize);

  uint64_t first_key = leaf_slot_key(page, live[split]);
  for (size_t index = split; index < live.size(); ++index) {
    uint32_t i = live[index];
    uint64_t key = leaf_slot_key(page, i);
    uint32_t flags = leaf_slot_flags(page, i);
    uint32_t length = leaf_slot_length(page, i);
    uint32_t offset = leaf_slot_offset(page, i);
    uint32_t right_count = leaf_slot_count(right_page);
    if (flags & kSlotOverflow) {
      // The chain of an overflow row is moved to the new page together with
      // the slot.
      leaf_set_slot(right_page, right_count, key, offset, length, flags);
      leaf_set_slot_count(right_page, right_count + 1);
    } else {
      leaf_append_slot(right_page, key, page + offset, length);
    }
  }
  storage_->mark_dirty(right);
  storage_->unpin_page(right);

  leaf_set_slot_count(page, mid);
  leaf_compact(page, false);
  storage_->mark_dirty(leaf);
  storage_->unpin_page(leaf);

  *right_leaf = right;
  *separator = first_key;
  return Status::Ok();
}

Status BTree::split_internal(uint64_t page_id, uint64_t* right_page,
                              uint64_t* separator, uint32_t mid) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(page_id, &page);
  if (!status.ok()) return status;
  uint32_t count = internal_slot_count(page);
  if (count < 2) {
    storage_->unpin_page(page_id);
    return Status::Error(ErrorCode::kInternalError,
                         "cannot split a single separator page");
  }
  if (mid < 1) mid = 1;
  if (mid > count - 1) mid = count - 1;

  status = storage_->note_modification(page_id, page);
  if (!status.ok()) {
    storage_->unpin_page(page_id);
    return status;
  }

  uint64_t right = 0;
  status = storage_->allocate_page(PageType::kInternal, &right);
  if (!status.ok()) {
    storage_->unpin_page(page_id);
    return status;
  }
  uint8_t* right_data = nullptr;
  status = storage_->fetch_page(right, &right_data);
  if (!status.ok()) {
    storage_->unpin_page(page_id);
    return status;
  }
  memset(right_data, 0, kPageSize);
  store_u32(right_data + 0, PageType::kInternal);
  store_u32(right_data + 4, static_cast<uint32_t>(right));

  uint64_t boundary = internal_slot_key(page, mid);
  uint32_t old_rightmost = internal_rightmost(page);

  internal_set_slot_count(page, mid);
  internal_set_rightmost(page, internal_slot_child(page, mid));

  uint32_t right_count = 0;
  for (uint32_t i = mid + 1; i < count; ++i) {
    internal_set_slot(right_data, right_count, internal_slot_key(page, i),
                      internal_slot_child(page, i));
    ++right_count;
  }
  internal_set_slot_count(right_data, right_count);
  internal_set_rightmost(right_data, old_rightmost);
  storage_->mark_dirty(right);
  storage_->unpin_page(right);

  storage_->mark_dirty(page_id);
  storage_->unpin_page(page_id);

  *right_page = right;
  *separator = boundary;
  return Status::Ok();
}
Status BTree::insert_into_parent(const std::vector<uint64_t>& path,
                                  uint64_t left_child, uint64_t separator_key,
                                  uint64_t right_child) {
  if (path.empty()) {
    uint64_t new_root = 0;
    Status status = storage_->allocate_page(PageType::kInternal, &new_root);
    if (!status.ok()) return status;
    uint8_t* page = nullptr;
    status = storage_->fetch_page(new_root, &page);
    if (!status.ok()) return status;
    memset(page, 0, kPageSize);
    store_u32(page + 0, PageType::kInternal);
    store_u32(page + 4, static_cast<uint32_t>(new_root));
    internal_set_slot(page, 0, separator_key, static_cast<uint32_t>(left_child));
    internal_set_slot_count(page, 1);
    internal_set_rightmost(page, static_cast<uint32_t>(right_child));
    storage_->mark_dirty(new_root);
    storage_->unpin_page(new_root);
    root_page_ = new_root;
    return Status::Ok();
  }

  uint64_t parent_id = path.back();
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(parent_id, &page);
  if (!status.ok()) return status;
  status = storage_->note_modification(parent_id, page);
  if (!status.ok()) {
    storage_->unpin_page(parent_id);
    return status;
  }
  uint32_t count = internal_slot_count(page);
  uint32_t position = count;
  for (uint32_t i = 0; i < count; ++i) {
    if (internal_slot_child(page, i) == static_cast<uint32_t>(left_child)) {
      position = i;
      break;
    }
  }
  if (position == count) {
    if (internal_rightmost(page) != static_cast<uint32_t>(left_child)) {
      storage_->unpin_page(parent_id);
      return Status::Error(ErrorCode::kInternalError,
                           "parent page does not reference the left child");
    }
  }

  uint32_t slots_end = kInternalHeaderSize + (count + 1) * kInternalSlotSize;
  if (slots_end > kPageSize) {
    // The parent is full.  It is split before the new separator is added, the
    // separator is then placed in the half that owns the child.
    uint32_t split_at = count / 2;
    if (split_at < 1) split_at = 1;
    if (split_at > count - 1) split_at = count - 1;
    // The child that was split must not become the rightmost child of the
    // left half, the two new slots would straddle the two halves.
    bool found = false;
    for (uint32_t candidate = split_at; candidate < count; ++candidate) {
      if (internal_slot_child(page, candidate) != static_cast<uint32_t>(left_child)) {
        split_at = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      for (uint32_t candidate = split_at; candidate > 0; --candidate) {
        if (internal_slot_child(page, candidate) != static_cast<uint32_t>(left_child)) {
          split_at = candidate;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      storage_->unpin_page(parent_id);
      return Status::Error(ErrorCode::kInternalError,
                           "the parent page cannot be split");
    }
    storage_->unpin_page(parent_id);
    uint64_t new_right = 0;
    uint64_t boundary = 0;
    status = split_internal(parent_id, &new_right, &boundary, split_at);
    if (!status.ok()) return status;
    // The pair of slots goes into the half that owns the child.
    if (position < split_at) {
      status = insert_pair_into_page(parent_id, position, left_child,
                                     separator_key, right_child);
    } else if (position == count) {
      // The child was the rightmost child, it belongs to the new right page.
      uint8_t* right_page = nullptr;
      status = storage_->fetch_page(new_right, &right_page);
      if (!status.ok()) return status;
      uint32_t right_count = internal_slot_count(right_page);
      storage_->unpin_page(new_right);
      status = insert_pair_into_page(new_right, right_count, left_child,
                                     separator_key, right_child);
    } else {
      status = insert_pair_into_page(new_right, position - split_at - 1,
                                     left_child, separator_key, right_child);
    }
    if (!status.ok()) return status;
    std::vector<uint64_t> parent_path(path.begin(), path.end() - 1);
    return insert_into_parent(parent_path, parent_id, boundary, new_right);
  }

  storage_->unpin_page(parent_id);
  return insert_pair_into_page(parent_id, position, left_child, separator_key,
                               right_child);
}

// Adds the separator of a split to an internal page.  The slot that pointed
// to the child that was split is replaced by two slots, the second one points
// to the new sibling.  `position` is the index of that slot, the number of the
// slots of the page when the child was the rightmost child.
Status BTree::insert_pair_into_page(uint64_t page_id, uint32_t position,
                                     uint64_t left_child, uint64_t separator_key,
                                     uint64_t right_child) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(page_id, &page);
  if (!status.ok()) return status;
  status = storage_->note_modification(page_id, page);
  if (!status.ok()) {
    storage_->unpin_page(page_id);
    return status;
  }
  uint32_t count = internal_slot_count(page);
  if (position >= count) {
    // The child was the rightmost child of the page.
    internal_set_slot(page, count, separator_key,
                      static_cast<uint32_t>(left_child));
    internal_set_slot_count(page, count + 1);
    internal_set_rightmost(page, static_cast<uint32_t>(right_child));
  } else {
    uint64_t old_key = internal_slot_key(page, position);
    for (uint32_t i = count; i > position + 1; --i) {
      internal_set_slot(page, i, internal_slot_key(page, i - 1),
                        internal_slot_child(page, i - 1));
    }
    internal_set_slot(page, position, separator_key,
                      static_cast<uint32_t>(left_child));
    internal_set_slot(page, position + 1, old_key,
                      static_cast<uint32_t>(right_child));
    internal_set_slot_count(page, count + 1);
  }
  storage_->mark_dirty(page_id);
  storage_->unpin_page(page_id);
  return Status::Ok();
}

Status BTree::insert(uint64_t key, const std::string& payload) {
  if (root_page_ == 0) {
    uint64_t root = 0;
    Status status = storage_->allocate_page(PageType::kLeaf, &root);
    if (!status.ok()) return status;
    uint8_t* page = nullptr;
    status = storage_->fetch_page(root, &page);
    if (!status.ok()) return status;
    memset(page, 0, kPageSize);
    store_u32(page + 0, PageType::kLeaf);
    store_u32(page + 4, static_cast<uint32_t>(root));
    leaf_set_heap_top(page, kPageSize);
    storage_->mark_dirty(root);
    storage_->unpin_page(root);
    root_page_ = root;
  }
  std::vector<uint64_t> path;
  uint64_t leaf = 0;
  Status status = find_leaf(key, &path, &leaf);
  if (!status.ok()) return status;

  status = leaf_insert(leaf, key, payload, false, false);
  if (status.ok()) return status;
  if (status.code() != ErrorCode::kStorageCorrupt) return status;

  uint64_t right = 0;
  uint64_t separator = 0;
  status = split_leaf(leaf, &right, &separator);
  if (!status.ok()) return status;
  status = insert_into_parent(path, leaf, separator, right);
  if (!status.ok()) return status;
  uint64_t target = (key >= separator) ? right : leaf;
  status = leaf_insert(target, key, payload, false, true);
  if (!status.ok()) return status;
  return Status::Ok();
}

Status BTree::lookup(uint64_t key, std::string* payload) {
  if (root_page_ == 0) {
    return Status::Error(ErrorCode::kInternalError, "no such key");
  }
  std::vector<uint64_t> path;
  uint64_t leaf = 0;
  Status status = find_leaf(key, &path, &leaf);
  if (!status.ok()) return status;
  uint8_t* page = nullptr;
  status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t count = leaf_slot_count(page);
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(count) - 1;
  while (low <= high) {
    int64_t mid = (low + high) / 2;
    uint64_t mid_key = leaf_slot_key(page, static_cast<uint32_t>(mid));
    if (mid_key == key) {
      uint32_t flags = leaf_slot_flags(page, static_cast<uint32_t>(mid));
      if (flags & kSlotDeleted) {
        storage_->unpin_page(leaf);
        return Status::Error(ErrorCode::kInternalError, "no such key");
      }
      status = read_payload(page, static_cast<uint32_t>(mid), payload);
      storage_->unpin_page(leaf);
      return status;
    }
    if (mid_key < key) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  storage_->unpin_page(leaf);
  return Status::Error(ErrorCode::kInternalError, "no such key");
}

Status BTree::update(uint64_t key, const std::string& payload) {
  if (root_page_ == 0) {
    return Status::Error(ErrorCode::kInternalError, "no such key");
  }
  std::vector<uint64_t> path;
  uint64_t leaf = 0;
  Status status = find_leaf(key, &path, &leaf);
  if (!status.ok()) return status;
  uint8_t* page = nullptr;
  status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t count = leaf_slot_count(page);
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(count) - 1;
  while (low <= high) {
    int64_t mid = (low + high) / 2;
    uint64_t mid_key = leaf_slot_key(page, static_cast<uint32_t>(mid));
    if (mid_key == key) {
      uint32_t flags = leaf_slot_flags(page, static_cast<uint32_t>(mid));
      storage_->unpin_page(leaf);
      if (flags & kSlotDeleted) {
        return Status::Error(ErrorCode::kInternalError, "no such key");
      }
      return write_payload(leaf, static_cast<uint32_t>(mid), payload);
    }
    if (mid_key < key) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  storage_->unpin_page(leaf);
  return Status::Error(ErrorCode::kInternalError, "no such key");
}

Status BTree::remove(uint64_t key) {
  if (root_page_ == 0) {
    return Status::Error(ErrorCode::kInternalError, "no such key");
  }
  std::vector<uint64_t> path;
  uint64_t leaf = 0;
  Status status = find_leaf(key, &path, &leaf);
  if (!status.ok()) return status;
  uint8_t* page = nullptr;
  status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t count = leaf_slot_count(page);
  int64_t low = 0;
  int64_t high = static_cast<int64_t>(count) - 1;
  while (low <= high) {
    int64_t mid = (low + high) / 2;
    uint64_t mid_key = leaf_slot_key(page, static_cast<uint32_t>(mid));
    if (mid_key == key) {
      uint32_t flags = leaf_slot_flags(page, static_cast<uint32_t>(mid));
      if (flags & kSlotDeleted) {
        storage_->unpin_page(leaf);
        return Status::Error(ErrorCode::kInternalError, "no such key");
      }
      status = storage_->note_modification(leaf, page);
      if (!status.ok()) {
        storage_->unpin_page(leaf);
        return status;
      }
      leaf_set_slot(page, static_cast<uint32_t>(mid), mid_key,
                    leaf_slot_offset(page, static_cast<uint32_t>(mid)),
                    leaf_slot_length(page, static_cast<uint32_t>(mid)),
                    flags | kSlotDeleted);
      storage_->mark_dirty(leaf);
      check_page(leaf, page);
      storage_->unpin_page(leaf);
      return Status::Ok();
    }
    if (mid_key < key) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  storage_->unpin_page(leaf);
  return Status::Error(ErrorCode::kInternalError, "no such key");
}

Status BTree::count(uint64_t* count) {
  *count = 0;
  bool stopped = false;
  Status status = scan([count](uint64_t, const std::string&) {
    ++(*count);
    return true;
  });
  (void)stopped;
  return status;
}

Status BTree::scan_page(uint64_t page_id, const ScanCallback& visitor,
                         bool* stopped) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(page_id, &page);
  if (!status.ok()) return status;
  uint32_t type = load_u32(page);
  if (type != PageType::kLeaf && type != PageType::kInternal) {
    // A page of an unknown type must not be read as a separator page, its
    // children would send the scan into a loop.
    storage_->unpin_page(page_id);
    return Status::Error(ErrorCode::kStorageCorrupt,
                         "the tree points at a page of an unknown type");
  }
  if (type == PageType::kLeaf) {
    uint32_t count = leaf_slot_count(page);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t flags = leaf_slot_flags(page, i);
      if (flags & kSlotDeleted) continue;
      uint64_t key = leaf_slot_key(page, i);
      std::string payload;
      status = read_payload(page, i, &payload);
      if (!status.ok()) {
        storage_->unpin_page(page_id);
        return status;
      }
      bool keep_going = visitor(key, payload);
      if (!keep_going) {
        *stopped = true;
        break;
      }
    }
    storage_->unpin_page(page_id);
    return Status::Ok();
  }
  uint32_t count = internal_slot_count(page);
  std::vector<uint32_t> children;
  children.reserve(count + 1);
  for (uint32_t i = 0; i < count; ++i) {
    children.push_back(internal_slot_child(page, i));
  }
  children.push_back(internal_rightmost(page));
  storage_->unpin_page(page_id);
  for (uint32_t child : children) {
    status = scan_page(child, visitor, stopped);
    if (!status.ok()) return status;
    if (*stopped) return Status::Ok();
  }
  return Status::Ok();
}

Status BTree::scan(const ScanCallback& visitor) {
  if (root_page_ == 0) return Status::Ok();
  bool stopped = false;
  ++scan_depth_;
  Status status = scan_page(root_page_, visitor, &stopped);
  --scan_depth_;
  return status;
}

}  // namespace mvdb
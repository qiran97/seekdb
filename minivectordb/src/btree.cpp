// B+tree implementation used as the clustered index of every table.
#include <algorithm>
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
// slots.  The slot array itself is preserved.
void leaf_compact(uint8_t* page) {
  uint32_t count = leaf_slot_count(page);
  struct Entry {
    uint64_t key;
    uint32_t length;
    uint32_t flags;
    uint32_t offset;
  };
  std::vector<Entry> entries;
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t flags = leaf_slot_flags(page, i);
    if (flags & kSlotDeleted) continue;
    Entry entry;
    entry.key = leaf_slot_key(page, i);
    entry.length = leaf_slot_length(page, i);
    entry.flags = flags;
    entry.offset = leaf_slot_offset(page, i);
    entries.push_back(entry);
  }
  uint32_t heap_top = kPageSize;
  for (Entry& entry : entries) {
    if (entry.flags & kSlotOverflow) continue;  // payload lives elsewhere
    heap_top -= entry.length;
    if (entry.offset != heap_top && entry.length > 0) {
      memmove(page + heap_top, page + entry.offset, entry.length);
    }
    entry.offset = heap_top;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    leaf_set_slot(page, static_cast<uint32_t>(i), entries[i].key,
                  entries[i].offset, entries[i].length, entries[i].flags);
  }
  leaf_set_slot_count(page, static_cast<uint32_t>(entries.size()));
  leaf_set_heap_top(page, heap_top);
}

// Writes a payload into an overflow chain and returns the first page id.
Status write_overflow_chain(Storage* storage, const std::string& payload,
                             uint64_t* first_page) {
  uint64_t chain = 0;
  uint64_t previous = 0;
  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t overflow_page_id = 0;
    Status status = storage->allocate_page(PageType::kOverflow, &overflow_page_id);
    if (!status.ok()) return status;
    uint8_t overflow_page[kPageSize];
    memset(overflow_page, 0, kPageSize);
    store_u32(overflow_page + 0, PageType::kOverflow);
    store_u32(overflow_page + 4, static_cast<uint32_t>(overflow_page_id));
    store_u32(overflow_page + 8, static_cast<uint32_t>(chain));
    uint32_t chunk = static_cast<uint32_t>(
        std::min<size_t>(payload.size() - offset, kPageSize - kOverflowHeaderSize));
    memcpy(overflow_page + kOverflowHeaderSize, payload.data() + offset, chunk);
    store_u32(overflow_page + kOverflowUsedOffset, chunk);
    status = storage->write_page(overflow_page_id, overflow_page);
    if (!status.ok()) return status;
    if (previous == 0) chain = overflow_page_id;
    previous = overflow_page_id;
    offset += chunk;
  }
  *first_page = chain;
  return Status::Ok();
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
  payload->assign(length, '\0');
  if (length == 0) return Status::Ok();
  if (flags & kSlotOverflow) {
    uint64_t next = offset;
    uint32_t done = 0;
    while (done < length && next != 0) {
      uint8_t overflow_page[kPageSize];
      Status status = storage_->read_page(next, overflow_page);
      if (!status.ok()) return status;
      uint32_t used = load_u32(overflow_page + kOverflowUsedOffset);
      uint32_t chunk = std::min<uint32_t>(used, length - done);
      if (chunk > 0) memcpy(&(*payload)[done], overflow_page + kOverflowHeaderSize, chunk);
      done += chunk;
      next = load_u32(overflow_page + 8);
    }
    if (done != length) {
      return Status::Error(ErrorCode::kStorageCorrupt,
                           "truncated overflow chain for row");
    }
    return Status::Ok();
  }
  memcpy(&(*payload)[0], page + offset, length);
  return Status::Ok();
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
      uint8_t overflow_page[kPageSize];
      Status read_status = storage_->read_page(next, overflow_page);
      if (!read_status.ok()) break;
      uint64_t following = load_u32(overflow_page + 8);
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
    leaf_compact(page);
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
                          bool replace) {
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
    leaf_set_slot(page, count, key, static_cast<uint32_t>(chain), length,
                  kSlotOverflow);
    leaf_set_slot_count(page, count + 1);
    storage_->mark_dirty(leaf);
    storage_->unpin_page(leaf);
    return Status::Ok();
  }

  uint32_t slots_end = kLeafHeaderSize + count * kLeafSlotSize;
  uint32_t heap_top = leaf_heap_top(page);
  if (heap_top == 0) heap_top = kPageSize;
  uint32_t needed = kLeafSlotSize + length;
  if (heap_top - slots_end < needed) {
    leaf_compact(page);
    count = leaf_slot_count(page);
    slots_end = kLeafHeaderSize + count * kLeafSlotSize;
    heap_top = leaf_heap_top(page);
    if (heap_top - slots_end < needed) {
      storage_->unpin_page(leaf);
      return Status::Error(ErrorCode::kStorageCorrupt, "leaf page is full");
    }
  }
  status = storage_->note_modification(leaf, page);
  if (!status.ok()) {
    storage_->unpin_page(leaf);
    return status;
  }
  uint32_t offset = heap_top - length;
  memcpy(page + offset, payload.data(), length);
  leaf_set_slot(page, count, key, offset, length, 0);
  leaf_set_slot_count(page, count + 1);
  leaf_set_heap_top(page, offset);
  storage_->mark_dirty(leaf);
  storage_->unpin_page(leaf);
  return Status::Ok();
}

Status BTree::split_leaf(uint64_t leaf, uint64_t* right_leaf, uint64_t* separator) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(leaf, &page);
  if (!status.ok()) return status;
  uint32_t count = leaf_slot_count(page);
  if (count < 2) {
    storage_->unpin_page(leaf);
    return Status::Error(ErrorCode::kInternalError, "cannot split a single slot leaf");
  }
  size_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    total += leaf_slot_length(page, i) + kLeafSlotSize;
  }
  size_t half = total / 2;
  size_t accumulated = 0;
  uint32_t mid = 1;
  for (uint32_t i = 0; i < count; ++i) {
    accumulated += leaf_slot_length(page, i) + kLeafSlotSize;
    if (accumulated >= half) {
      mid = i + 1;
      break;
    }
  }
  if (mid >= count) mid = count - 1;
  if (mid == 0) mid = 1;

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

  uint64_t first_key = 0;
  for (uint32_t i = mid; i < count; ++i) {
    uint64_t key = leaf_slot_key(page, i);
    uint32_t flags = leaf_slot_flags(page, i);
    uint32_t length = leaf_slot_length(page, i);
    uint32_t offset = leaf_slot_offset(page, i);
    if (i == mid) first_key = key;
    uint32_t right_count = leaf_slot_count(right_page);
    if (flags & kSlotOverflow) {
      leaf_set_slot(right_page, right_count, key, offset, length, flags);
      leaf_set_slot_count(right_page, right_count + 1);
    } else {
      leaf_append_slot(right_page, key, page + offset, length);
    }
  }
  storage_->mark_dirty(right);
  storage_->unpin_page(right);

  leaf_set_slot_count(page, mid);
  leaf_compact(page);
  storage_->mark_dirty(leaf);
  storage_->unpin_page(leaf);

  *right_leaf = right;
  *separator = first_key;
  return Status::Ok();
}

Status BTree::split_internal(uint64_t page_id, uint64_t* right_page,
                              uint64_t* separator) {
  uint8_t* page = nullptr;
  Status status = storage_->fetch_page(page_id, &page);
  if (!status.ok()) return status;
  uint32_t count = internal_slot_count(page);
  if (count < 2) {
    storage_->unpin_page(page_id);
    return Status::Error(ErrorCode::kInternalError,
                         "cannot split a single separator page");
  }
  uint32_t mid = count / 2;
  if (mid == 0) mid = 1;
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
  bool is_rightmost = false;
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
    is_rightmost = true;
  }

  uint32_t slots_end = kInternalHeaderSize + (count + 1) * kInternalSlotSize;
  if (slots_end > kPageSize) {
    storage_->unpin_page(parent_id);
    uint64_t new_right = 0;
    uint64_t boundary = 0;
    status = split_internal(parent_id, &new_right, &boundary);
    if (!status.ok()) return status;
    std::vector<uint64_t> parent_path(path.begin(), path.end() - 1);
    return insert_into_parent(parent_path, parent_id, boundary, new_right);
  }

  if (is_rightmost) {
    internal_set_slot(page, count, separator_key, static_cast<uint32_t>(left_child));
    internal_set_slot_count(page, count + 1);
    internal_set_rightmost(page, static_cast<uint32_t>(right_child));
  } else {
    uint64_t old_key = internal_slot_key(page, position);
    uint32_t old_child = internal_slot_child(page, position);
    for (uint32_t i = count; i > position + 1; --i) {
      internal_set_slot(page, i, internal_slot_key(page, i - 1),
                        internal_slot_child(page, i - 1));
    }
    internal_set_slot(page, position, separator_key,
                      static_cast<uint32_t>(left_child));
    internal_set_slot(page, position + 1, old_key, old_child);
    internal_set_slot_count(page, count + 1);
  }
  storage_->mark_dirty(parent_id);
  storage_->unpin_page(parent_id);
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

  status = leaf_insert(leaf, key, payload, false);
  if (status.ok()) return status;
  if (status.code() != ErrorCode::kStorageCorrupt) return status;

  uint64_t right = 0;
  uint64_t separator = 0;
  status = split_leaf(leaf, &right, &separator);
  if (!status.ok()) return status;
  status = insert_into_parent(path, leaf, separator, right);
  if (!status.ok()) return status;
  uint64_t target = (key >= separator) ? right : leaf;
  status = leaf_insert(target, key, payload, false);
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
  return scan_page(root_page_, visitor, &stopped);
}

}  // namespace mvdb
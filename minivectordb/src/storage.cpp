#include "storage.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace mvdb {

namespace {

constexpr char kMagic[8] = {'M', 'V', 'D', 'B', '0', '0', '1', '\0'};
constexpr uint32_t kFormatVersion = 1;

// Offsets inside the leaf page.
constexpr uint32_t kLeafSlotCountOffset = 16;
constexpr uint32_t kLeafHeapTopOffset = 20;
// Offsets inside the internal page.
constexpr uint32_t kInternalSlotCountOffset = 16;
constexpr uint32_t kInternalRightmostOffset = 20;
// Offsets inside the overflow page.
constexpr uint32_t kOverflowUsedOffset = 16;
// Offsets inside the catalog page.
constexpr uint32_t kCatalogLengthOffset = 16;

// Offsets inside the database header page.
constexpr uint32_t kDbVersionOffset = 8;
constexpr uint32_t kDbPageSizeOffset = 12;
constexpr uint32_t kDbNextPageOffset = 16;
constexpr uint32_t kDbPageCountOffset = 24;
constexpr uint32_t kDbFreeListOffset = 32;
constexpr uint32_t kDbCatalogOffset = 40;
constexpr uint32_t kDbLastCommitOffset = 48;

uint32_t crc32_of(const uint8_t* data, size_t length) {
  static uint32_t table[256];
  static bool initialized = false;
  if (!initialized) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    initialized = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

Status io_error(const std::string& what) {
  return Status::Error(ErrorCode::kIoError,
                       what + ": " + std::string(strerror(errno)));
}

// Creates the directory and all of its parents.
Status ensure_directory(const std::string& path) {
  if (path.empty()) return Status::Ok();
  std::string current;
  for (size_t i = 0; i <= path.size(); ++i) {
    bool end = (i == path.size());
    if (!end && path[i] != '/') {
      current.push_back(path[i]);
      continue;
    }
    if (!current.empty() && current != "/") {
      if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
        return io_error("cannot create directory " + current);
      }
    }
    if (!end) current.push_back('/');
  }
  return Status::Ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// FileStore
// ---------------------------------------------------------------------------

FileStore::~FileStore() {
  if (fd_ >= 0) ::close(fd_);
}

Status FileStore::open(const std::string& path, bool create) {
  int flags = O_RDWR;
  if (create) flags |= O_CREAT;
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) return io_error("cannot open " + path);
  path_ = path;
  struct stat st;
  if (::fstat(fd_, &st) != 0) return io_error("cannot stat " + path);
  size_ = static_cast<uint64_t>(st.st_size);
  return Status::Ok();
}

Status FileStore::read_at(uint8_t* buffer, uint64_t offset, uint32_t length) {
  if (fd_ < 0) return Status::Error(ErrorCode::kIoError, "file is not open");
  uint32_t done = 0;
  while (done < length) {
    ssize_t n = ::pread(fd_, buffer + done, length - done,
                        static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return io_error("read failed on " + path_);
    }
    if (n == 0) {
      // Reading past the end of the file yields zeroes.
      memset(buffer + done, 0, length - done);
      break;
    }
    done += static_cast<uint32_t>(n);
  }
  return Status::Ok();
}

Status FileStore::write_at(const uint8_t* buffer, uint64_t offset, uint32_t length) {
  if (fd_ < 0) return Status::Error(ErrorCode::kIoError, "file is not open");
  uint32_t done = 0;
  while (done < length) {
    ssize_t n = ::pwrite(fd_, buffer + done, length - done,
                         static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return io_error("write failed on " + path_);
    }
    done += static_cast<uint32_t>(n);
  }
  if (offset + length > size_) size_ = offset + length;
  return Status::Ok();
}

Status FileStore::append(const uint8_t* buffer, uint32_t length) {
  if (fd_ < 0) return Status::Error(ErrorCode::kIoError, "file is not open");
  uint32_t done = 0;
  while (done < length) {
    ssize_t n = ::write(fd_, buffer + done, length - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return io_error("append failed on " + path_);
    }
    done += static_cast<uint32_t>(n);
  }
  size_ += length;
  return Status::Ok();
}

Status FileStore::sync() {
  if (fd_ < 0) return Status::Ok();
  if (::fdatasync(fd_) != 0) return io_error("fdatasync failed on " + path_);
  return Status::Ok();
}

Status FileStore::truncate(uint64_t size) {
  if (fd_ < 0) return Status::Error(ErrorCode::kIoError, "file is not open");
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
    return io_error("ftruncate failed on " + path_);
  }
  size_ = size;
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// UndoLog
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t kUndoRecordMagic = 0x4D564457u;  // "MVDB"
constexpr uint32_t kUndoRecordHeaderSize = 32;
}  // namespace

Status UndoLog::open(const std::string& path) {
  path_ = path;
  int flags = O_RDWR | O_CREAT;
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) return io_error("cannot open undo log " + path);
  struct stat st;
  if (::fstat(fd_, &st) != 0) return io_error("cannot stat undo log " + path);
  // The log keeps its content across restarts: the engine replays it before
  // the database is opened and truncates it afterwards.
  file_size_ = static_cast<uint64_t>(st.st_size);
  buffer_.clear();
  buffered_ = 0;
  return Status::Ok();
}

Status UndoLog::append_raw(uint32_t type, uint64_t txn_id, uint64_t page_id,
                           const uint8_t* payload, uint32_t length) {
  uint8_t header[kUndoRecordHeaderSize];
  store_u32(header, kUndoRecordMagic);
  store_u32(header + 4, type);
  store_u64(header + 8, txn_id);
  store_u64(header + 16, page_id);
  store_u32(header + 24, length);
  uint32_t crc = crc32_of(header, 24);
  crc = crc32_of(payload, length) ^ crc;
  store_u32(header + 28, crc);
  buffer_.insert(buffer_.end(), header, header + kUndoRecordHeaderSize);
  buffer_.insert(buffer_.end(), payload, payload + length);
  buffered_ += kUndoRecordHeaderSize + length;
  return Status::Ok();
}

Status UndoLog::append_page_undo(uint64_t txn_id, uint64_t page_id,
                                  const uint8_t* image) {
  return append_raw(RecordType::kRecordPageUndo, txn_id, page_id, image,
                     kPageSize);
}

Status UndoLog::append_marker(uint32_t type, uint64_t txn_id) {
  return append_raw(type, txn_id, 0, nullptr, 0);
}

Status UndoLog::flush() {
  if (buffer_.empty()) return Status::Ok();
  uint32_t done = 0;
  while (done < buffer_.size()) {
    ssize_t n = ::pwrite(fd_, buffer_.data() + done, buffer_.size() - done,
                          static_cast<off_t>(file_size_ + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return io_error("write failed on " + path_);
    }
    done += static_cast<uint32_t>(n);
  }
  file_size_ += buffer_.size();
  buffer_.clear();
  buffered_ = 0;
  return Status::Ok();
}

Status UndoLog::sync() {
  Status status = flush();
  if (!status.ok()) return status;
  return Status::Ok();
}

Status UndoLog::reset() {
  buffer_.clear();
  buffered_ = 0;
  if (fd_ >= 0) {
    if (::ftruncate(fd_, 0) != 0) return io_error("cannot reset undo log");
    file_size_ = 0;
  }
  return Status::Ok();
}

Status UndoLog::truncate_to(uint64_t offset) {
  Status status = flush();
  if (!status.ok()) return status;
  if (fd_ < 0) return Status::Ok();
  if (offset > file_size_) offset = file_size_;
  if (::ftruncate(fd_, static_cast<off_t>(offset)) != 0) {
    return io_error("cannot truncate undo log");
  }
  file_size_ = offset;
  return Status::Ok();
}

Status UndoLog::records_from(uint64_t offset, std::vector<UndoRecord>* records) {
  records->clear();
  if (fd_ < 0) return Status::Ok();
  uint64_t total = file_size_ + buffered_;
  if (offset >= total) return Status::Ok();
  std::vector<uint8_t> chunk;
  uint64_t position = offset;
  uint8_t header[kUndoRecordHeaderSize];
  while (position + kUndoRecordHeaderSize <= total) {
    uint64_t available = total - position;
    uint32_t to_read = static_cast<uint32_t>(
        std::min<uint64_t>(available, kUndoRecordHeaderSize));
    if (to_read < kUndoRecordHeaderSize) break;
    Status status = read_exact(position, header, kUndoRecordHeaderSize);
    if (!status.ok()) break;
    if (load_u32(header) != kUndoRecordMagic) break;
    uint32_t type = load_u32(header + 4);
    uint64_t txn_id = load_u64(header + 8);
    uint64_t page_id = load_u64(header + 16);
    uint32_t length = load_u32(header + 24);
    UndoRecord record;
    record.type = type;
    record.txn_id = txn_id;
    record.page_id = page_id;
    if (length > 0) {
      record.payload.resize(length);
      status = read_exact(position + kUndoRecordHeaderSize, record.payload.data(), length);
      if (!status.ok()) break;
    }
    uint32_t crc = load_u32(header + 28);
    uint32_t check = crc32_of(header, 24);
    check = crc32_of(record.payload.data(), record.payload.size()) ^ check;
    if (check != crc) break;
    records->push_back(std::move(record));
    position += kUndoRecordHeaderSize + length;
  }
  return Status::Ok();
}

Status UndoLog::read_exact(uint64_t offset, uint8_t* buffer, uint32_t length) {
  // The buffered bytes live behind the file content.
  uint64_t file_end = file_size_;
  if (offset < file_end) {
    uint32_t from_file = static_cast<uint32_t>(
        std::min<uint64_t>(length, file_end - offset));
    Status status = pread_exact(offset, buffer, from_file);
    if (!status.ok()) return status;
    if (from_file == length) return Status::Ok();
    memmove(buffer, buffer + from_file, length - from_file);
    uint32_t rest = length - from_file;
    if (rest > buffered_) rest = buffered_;
    memcpy(buffer + (length - rest), buffer_.data(), rest);
    return Status::Ok();
  }
  uint64_t in_buffer = offset - file_end;
  if (in_buffer >= buffered_) return Status::Error(ErrorCode::kStorageCorrupt, "undo log read beyond end");
  uint32_t rest = static_cast<uint32_t>(
      std::min<uint64_t>(length, buffered_ - in_buffer));
  memcpy(buffer, buffer_.data() + in_buffer, rest);
  return Status::Ok();
}

Status UndoLog::pread_exact(uint64_t offset, uint8_t* buffer, uint32_t length) {
  uint32_t done = 0;
  while (done < length) {
    ssize_t n = ::pread(fd_, buffer + done, length - done, static_cast<off_t>(offset + done));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return Status::Error(ErrorCode::kStorageCorrupt, "short read from undo log");
    }
    done += static_cast<uint32_t>(n);
  }
  return Status::Ok();
}

Status UndoLog::replay(
    const std::function<bool(uint32_t, uint64_t, uint64_t, const uint8_t*, uint32_t)>&
        handler) {
  if (fd_ < 0) return Status::Ok();
  Status flush_status = flush();
  if (!flush_status.ok()) return flush_status;
  std::vector<UndoRecord> records;
  Status status = records_from(0, &records);
  if (!status.ok()) return status;
  for (const UndoRecord& record : records) {
    if (!handler(record.type, record.txn_id, record.page_id, record.payload.data(),
                 static_cast<uint32_t>(record.payload.size()))) {
      break;
    }
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// PageCache
// ---------------------------------------------------------------------------

Status PageCache::load_frame(uint64_t page_id, uint32_t* index) {
  auto it = lookup_.find(page_id);
  if (it != lookup_.end()) {
    *index = it->second;
    return Status::Ok();
  }
  // Find a victim frame.
  uint32_t victim = UINT32_MAX;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t i = 0; i < frames_.size(); ++i) {
    Frame& frame = frames_[i];
    if (!frame.valid) {
      victim = i;
      break;
    }
    if (frame.pin_count == 0 && frame.last_use < oldest) {
      oldest = frame.last_use;
      victim = i;
    }
  }
  if (victim == UINT32_MAX) {
    // Every frame is pinned: try to flush one of them.
    for (uint32_t i = 0; i < frames_.size(); ++i) {
      if (frames_[i].pin_count == 0) {
        victim = i;
        break;
      }
    }
  }
  if (victim == UINT32_MAX) {
    return Status::Error(ErrorCode::kInternalError,
                         "buffer pool exhausted: too many pinned pages");
  }
  if (frames_[victim].valid && frames_[victim].dirty) {
    Status status = evict_frame(victim);
    if (!status.ok()) return status;
  }
  Frame& frame = frames_[victim];
  // The frame may still carry the id of a page that it does not hold any
  // more, the mapping of that page must not be removed.
  uint64_t old_page_id = frame.valid ? frame.page_id : 0;
  if (!frame.valid) {
    frame.data.assign(kPageSize, 0);
    frame.valid = true;
  }
  lookup_.erase(page_id);
  if (old_page_id != 0) lookup_.erase(old_page_id);
  frame.page_id = page_id;
  frame.dirty = false;
  frame.pin_count = 0;
  frame.last_use = ++clock_;
  Status status = file_->read_at(frame.data.data(), page_id * kPageSize, kPageSize);
  if (!status.ok()) return status;
  lookup_[page_id] = victim;
  *index = victim;
  return Status::Ok();
}

Status PageCache::evict_frame(uint32_t index) {
  Frame& frame = frames_[index];
  Status status = flush_page(frame.page_id);
  if (!status.ok()) return status;
  lookup_.erase(frame.page_id);
  frame.valid = false;
  frame.page_id = 0;
  frame.dirty = false;
  frame.pin_count = 0;
  return Status::Ok();
}

Status PageCache::fetch(uint64_t page_id, uint8_t** data) {
  uint32_t index = 0;
  Status status = load_frame(page_id, &index);
  if (!status.ok()) return status;
  Frame& frame = frames_[index];
  frame.pin_count++;
  frame.last_use = ++clock_;
  *data = frame.data.data();
  return Status::Ok();
}

void PageCache::unpin(uint64_t page_id) {
  auto it = lookup_.find(page_id);
  if (it == lookup_.end()) return;
  Frame& frame = frames_[it->second];
  if (frame.pin_count > 0) frame.pin_count--;
}

void PageCache::mark_dirty(uint64_t page_id) {
  auto it = lookup_.find(page_id);
  if (it == lookup_.end()) return;
  frames_[it->second].dirty = true;
}

bool PageCache::is_dirty(uint64_t page_id) const {
  auto it = lookup_.find(page_id);
  if (it == lookup_.end()) return false;
  return frames_[it->second].dirty;
}

Status PageCache::flush_page(uint64_t page_id) {
  auto it = lookup_.find(page_id);
  if (it == lookup_.end()) return Status::Ok();
  Frame& frame = frames_[it->second];
  if (!frame.dirty) return Status::Ok();
  if (before_flush_) {
    Status hook_status = before_flush_();
    if (!hook_status.ok()) return hook_status;
  }
  Status status = file_->write_at(frame.data.data(), page_id * kPageSize, kPageSize);
  if (!status.ok()) return status;
  frame.dirty = false;
  return Status::Ok();
}

Status PageCache::flush_all() {
  for (uint32_t i = 0; i < frames_.size(); ++i) {
    if (!frames_[i].valid || !frames_[i].dirty) continue;
    Status status = flush_page(frames_[i].page_id);
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

void PageCache::drop(uint64_t page_id) {
  auto it = lookup_.find(page_id);
  if (it == lookup_.end()) return;
  Frame& frame = frames_[it->second];
  frame.valid = false;
  frame.dirty = false;
  frame.pin_count = 0;
  // The page id has to be cleared as well: the frame is reused for another
  // page later and the stale id would remove the mapping of that page.
  frame.page_id = 0;
  lookup_.erase(it);
}

size_t PageCache::dirty_count() const {
  size_t count = 0;
  for (const Frame& frame : frames_) {
    if (frame.valid && frame.dirty) ++count;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

Status Storage::open(const std::string& data_dir, size_t buffer_pool_bytes) {
  size_t frames = buffer_pool_bytes / kPageSize;
  if (frames < 16) frames = 16;
  cache_.set_capacity(frames);
  Status directory_status = ensure_directory(data_dir);
  if (!directory_status.ok()) return directory_status;
  std::string data_path = data_dir + "/mvdb.dat";
  std::string log_path = data_dir + "/mvdb.undo";
  Status status = file_.open(data_path, true);
  if (!status.ok()) return status;
  status = undo_log_.open(log_path);
  if (!status.ok()) return status;
  return load_db_header();
}

Status Storage::close() {
  Status status = cache_.flush_all();
  if (!status.ok()) return status;
  return file_.sync();
}

void Storage::begin_transaction(uint64_t txn_id) {
  current_txn_ = txn_id;
  logged_pages_.clear();
  has_changes_ = false;
}

void Storage::end_transaction() {
  current_txn_ = 0;
  logged_pages_.clear();
}

Status Storage::note_modification(uint64_t page_id, const uint8_t* image) {
  has_changes_ = true;
  if (current_txn_ == 0) return Status::Ok();
  if (logged_pages_.find(page_id) != logged_pages_.end()) return Status::Ok();
  logged_pages_.insert(page_id);
  return undo_log_.append_page_undo(current_txn_, page_id, image);
}

Status Storage::flush_undo_log() { return undo_log_.flush(); }

Status Storage::fetch_page(uint64_t page_id, uint8_t** data) {
  return cache_.fetch(page_id, data);
}

void Storage::unpin_page(uint64_t page_id) { cache_.unpin(page_id); }

void Storage::mark_dirty(uint64_t page_id) { cache_.mark_dirty(page_id); }

Status Storage::allocate_page(uint32_t type, uint64_t* page_id) {
  has_changes_ = true;
  uint64_t id = 0;
  if (free_list_head_ != 0) {
    uint8_t* page = nullptr;
    Status status = fetch_page(free_list_head_, &page);
    if (!status.ok()) return status;
    id = free_list_head_;
    free_list_head_ = load_u32(page + 8);
    store_u32(page + 0, PageType::kFree);
    mark_dirty(id);
    unpin_page(id);
    Status header_status = store_db_header();
    if (!header_status.ok()) return header_status;
  } else {
    id = next_page_id_++;
    page_count_ = id + 1;
    Status header_status = store_db_header();
    if (!header_status.ok()) return header_status;
  }
  uint8_t* page = nullptr;
  Status status = fetch_page(id, &page);
  if (!status.ok()) return status;
  // The before image of the page has to be logged, a rollback has to be able
  // to give the page back to the free list.
  status = note_modification(id, page);
  if (!status.ok()) {
    unpin_page(id);
    return status;
  }
  memset(page, 0, kPageSize);
  store_u32(page + 0, type);
  store_u32(page + 4, static_cast<uint32_t>(id));
  store_u32(page + 8, 0);
  mark_dirty(id);
  unpin_page(id);
  *page_id = id;
  return Status::Ok();
}

Status Storage::free_page(uint64_t page_id) {
  has_changes_ = true;
  uint8_t* page = nullptr;
  Status status = fetch_page(page_id, &page);
  if (!status.ok()) return status;
  status = note_modification(page_id, page);
  if (!status.ok()) {
    unpin_page(page_id);
    return status;
  }
  store_u32(page + 0, PageType::kFree);
  store_u32(page + 8, static_cast<uint32_t>(free_list_head_));
  free_list_head_ = page_id;
  mark_dirty(page_id);
  unpin_page(page_id);
  return store_db_header();
}

Status Storage::read_page(uint64_t page_id, uint8_t* buffer) {
  return file_.read_at(buffer, page_id * kPageSize, kPageSize);
}

Status Storage::write_page(uint64_t page_id, const uint8_t* buffer) {
  return file_.write_at(buffer, page_id * kPageSize, kPageSize);
}

Status Storage::load_db_header() {
  uint8_t page[kPageSize];
  Status status = file_.read_at(page, 0, kPageSize);
  if (!status.ok()) return status;
  bool fresh = memcmp(page, kMagic, sizeof(kMagic)) != 0;
  if (fresh) {
    memset(page, 0, kPageSize);
    memcpy(page, kMagic, sizeof(kMagic));
    store_u32(page + kDbVersionOffset, kFormatVersion);
    store_u32(page + kDbPageSizeOffset, kPageSize);
    store_u64(page + kDbNextPageOffset, 1);
    store_u64(page + kDbPageCountOffset, 1);
    store_u64(page + kDbFreeListOffset, 0);
    store_u64(page + kDbCatalogOffset, 0);
    store_u64(page + kDbLastCommitOffset, 0);
    status = file_.write_at(page, 0, kPageSize);
    if (!status.ok()) return status;
    next_page_id_ = 1;
    page_count_ = 1;
    free_list_head_ = 0;
    catalog_page_ = 0;
    last_committed_txn_ = 0;
    return Status::Ok();
  }
  if (load_u32(page + kDbPageSizeOffset) != kPageSize) {
    return Status::Error(ErrorCode::kStorageCorrupt,
                         "database page size mismatch, incompatible data file");
  }
  next_page_id_ = load_u64(page + kDbNextPageOffset);
  page_count_ = load_u64(page + kDbPageCountOffset);
  free_list_head_ = load_u64(page + kDbFreeListOffset);
  catalog_page_ = load_u64(page + kDbCatalogOffset);
  last_committed_txn_ = load_u64(page + kDbLastCommitOffset);
  return Status::Ok();
}

Status Storage::store_db_header() {
  uint8_t* page = nullptr;
  Status status = fetch_page(0, &page);
  if (!status.ok()) return status;
  status = note_modification(0, page);
  if (!status.ok()) {
    unpin_page(0);
    return status;
  }
  memcpy(page, kMagic, sizeof(kMagic));
  store_u32(page + kDbVersionOffset, kFormatVersion);
  store_u32(page + kDbPageSizeOffset, kPageSize);
  store_u64(page + kDbNextPageOffset, next_page_id_);
  store_u64(page + kDbPageCountOffset, page_count_);
  store_u64(page + kDbFreeListOffset, free_list_head_);
  store_u64(page + kDbCatalogOffset, catalog_page_);
  store_u64(page + kDbLastCommitOffset, last_committed_txn_);
  mark_dirty(0);
  unpin_page(0);
  return Status::Ok();
}

}  // namespace mvdb
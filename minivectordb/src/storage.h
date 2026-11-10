// Storage engine of MiniVectorDB.
//
// The database uses a single data file that holds every page (catalog, table
// trees and row payloads) plus a separate undo log.  Durability follows the
// "force at commit" policy:
//
//   1. every page that is modified inside a transaction gets its *before*
//      image written to the undo log before the modification happens,
//   2. a commit writes all dirty pages to the data file, fsyncs it and only
//      then appends the commit record to the undo log and fsyncs that.
//
// A crash therefore leaves at most one uncommitted transaction behind, and the
// undo log is sufficient to restore the pre-transaction image of every page.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstring>

#include "base.h"

namespace mvdb {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kLeafHeaderSize = 40;
constexpr uint32_t kLeafSlotSize = 24;
constexpr uint32_t kInternalHeaderSize = 40;
constexpr uint32_t kInternalSlotSize = 16;
constexpr uint32_t kOverflowHeaderSize = 24;
constexpr uint32_t kCatalogHeaderSize = 24;
constexpr uint32_t kDbHeaderSize = 128;

enum PageType : uint32_t {
  kFree = 1,
  kLeaf = 2,
  kInternal = 3,
  kOverflow = 4,
  kCatalog = 5,
};

enum LeafSlotFlags : uint32_t {
  kSlotDeleted = 1u << 0,
  kSlotOverflow = 1u << 1,
};

// Little endian accessors, the pages are stored in a byte buffer.
inline uint32_t load_u32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
inline uint64_t load_u64(const uint8_t* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
inline void store_u32(uint8_t* p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
inline void store_u64(uint8_t* p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

// A file backed byte range.
class FileStore {
 public:
  FileStore() = default;
  ~FileStore();
  FileStore(const FileStore&) = delete;
  FileStore& operator=(const FileStore&) = delete;

  Status open(const std::string& path, bool create);
  Status read_at(uint8_t* buffer, uint64_t offset, uint32_t length);
  Status write_at(const uint8_t* buffer, uint64_t offset, uint32_t length);
  Status append(const uint8_t* buffer, uint32_t length);
  Status sync();
  Status truncate(uint64_t size);
  uint64_t size() const { return size_; }
  const std::string& path() const { return path_; }
  bool is_open() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
  std::string path_;
  uint64_t size_ = 0;
};

// One record of the undo log.
struct UndoRecord {
  uint32_t type = 0;
  uint64_t txn_id = 0;
  uint64_t page_id = 0;
  std::vector<uint8_t> payload;
};

// The undo log.  Records are buffered in memory and flushed to the file
// before any dirty page is written to the data file.
class UndoLog {
 public:
  enum RecordType : uint32_t {
    kRecordPageUndo = 1,
    kRecordCommit = 2,
    kRecordRollback = 3,
  };

  Status open(const std::string& path);
  Status append_page_undo(uint64_t txn_id, uint64_t page_id,
                           const uint8_t* image);
  Status append_marker(uint32_t type, uint64_t txn_id);
  Status flush();
  Status sync();
  Status reset();
  // Invokes the handler for every valid record; returns false to stop.
  Status replay(
      const std::function<bool(uint32_t type, uint64_t txn_id, uint64_t page_id,
                               const uint8_t* payload, uint32_t length)>& handler);
  uint64_t bytes() const { return buffered_; }
  // Current end of the log (file bytes plus buffered bytes).
  uint64_t position() const { return file_size_ + buffered_; }
  // Reads every record starting at `offset`.
  Status records_from(uint64_t offset, std::vector<UndoRecord>* records);
  // Drops every record after `offset`.
  Status truncate_to(uint64_t offset);
  uint64_t file_size() const { return file_size_; }

 private:
  Status append_raw(uint32_t type, uint64_t txn_id, uint64_t page_id,
                    const uint8_t* payload, uint32_t length);
  Status read_exact(uint64_t offset, uint8_t* buffer, uint32_t length);
  Status pread_exact(uint64_t offset, uint8_t* buffer, uint32_t length);

  int fd_ = -1;
  std::string path_;
  std::vector<uint8_t> buffer_;
  uint64_t buffered_ = 0;
  uint64_t file_size_ = 0;
};

// Buffer pool with LRU eviction.  Frames are pinned by the caller.
class PageCache {
 public:
  PageCache(FileStore* file, size_t capacity_frames)
      : file_(file), capacity_frames_(capacity_frames) {
    frames_.resize(capacity_frames_);
  }

  Status fetch(uint64_t page_id, uint8_t** data);
  void unpin(uint64_t page_id);
  void mark_dirty(uint64_t page_id);
  bool is_dirty(uint64_t page_id) const;
  // Writes one dirty page to the data file.
  Status flush_page(uint64_t page_id);
  // Writes every dirty page.
  Status flush_all();
  void drop(uint64_t page_id);
  size_t dirty_count() const;
  void set_capacity(size_t frames) {
    capacity_frames_ = frames;
    frames_.resize(frames);
  }
  // Invoked before a dirty page is written to the data file.  The engine uses
  // it to make sure the undo log is complete on disk first.
  void set_before_flush(std::function<Status()> hook) { before_flush_ = hook; }

 private:
  struct Frame {
    uint64_t page_id = 0;
    bool valid = false;
    bool dirty = false;
    int pin_count = 0;
    uint64_t last_use = 0;
    std::vector<uint8_t> data;
  };

  Status load_frame(uint64_t page_id, uint32_t* index);
  Status evict_frame(uint32_t index);

  FileStore* file_;
  std::function<Status()> before_flush_;
  size_t capacity_frames_;
  std::vector<Frame> frames_;
  std::unordered_map<uint64_t, uint32_t> lookup_;
  uint64_t clock_ = 0;
};

// Owns the data file, the undo log and the page cache.
class Storage {
 public:
  Storage() = default;

  Status open(const std::string& data_dir, size_t buffer_pool_bytes);
  Status close();

  FileStore* file() { return &file_; }
  PageCache* cache() { return &cache_; }
  UndoLog* undo_log() { return &undo_log_; }

  // Transaction hooks -----------------------------------------------------
  void begin_transaction(uint64_t txn_id);
  void end_transaction();
  // Forgets which pages were already logged, so that the next statement logs
  // the before image of every page it modifies again.
  void reset_logged_pages() { logged_pages_.clear(); }
  uint64_t current_txn() const { return current_txn_; }
  bool in_transaction() const { return current_txn_ != 0; }
  // True when the running transaction modified at least one page.
  bool has_changes() const { return has_changes_; }
  // Must be called *before* a page is modified: stores the before image.
  Status note_modification(uint64_t page_id, const uint8_t* image);
  // Writes the pending undo records to the undo log file.
  Status flush_undo_log();

  // Page helpers ----------------------------------------------------------
  Status fetch_page(uint64_t page_id, uint8_t** data);
  void unpin_page(uint64_t page_id);
  void mark_dirty(uint64_t page_id);
  Status allocate_page(uint32_t type, uint64_t* page_id);
  Status free_page(uint64_t page_id);
  Status read_page(uint64_t page_id, uint8_t* buffer);
  Status write_page(uint64_t page_id, const uint8_t* buffer);

  // Database header -------------------------------------------------------
  uint64_t next_page_id() const { return next_page_id_; }
  void set_next_page_id(uint64_t value) { next_page_id_ = value; }
  uint64_t page_count() const { return page_count_; }
  void set_page_count(uint64_t value) { page_count_ = value; }
  uint64_t free_list_head() const { return free_list_head_; }
  void set_free_list_head(uint64_t value) { free_list_head_ = value; }
  uint64_t catalog_page() const { return catalog_page_; }
  void set_catalog_page(uint64_t value) { catalog_page_ = value; }
  uint64_t last_committed_txn() const { return last_committed_txn_; }
  void set_last_committed_txn(uint64_t value) { last_committed_txn_ = value; }

  Status load_db_header();
  Status store_db_header();

 private:
  FileStore file_;
  UndoLog undo_log_;
  PageCache cache_{&file_, 64};
  std::unordered_set<uint64_t> logged_pages_;
  uint64_t current_txn_ = 0;
  bool has_changes_ = false;
  uint64_t next_page_id_ = 1;
  uint64_t page_count_ = 1;
  uint64_t free_list_head_ = 0;
  uint64_t catalog_page_ = 0;
  uint64_t last_committed_txn_ = 0;
};

// A B+tree over 64 bit keys.  Leaf pages hold the row payloads inline; very
// large payloads are stored in an overflow chain.
class BTree {
 public:
  BTree(Storage* storage, uint64_t root_page)
      : storage_(storage), root_page_(root_page) {}

  uint64_t root_page() const { return root_page_; }
  void set_root_page(uint64_t value) { root_page_ = value; }

  Status insert(uint64_t key, const std::string& payload);
  Status lookup(uint64_t key, std::string* payload);
  Status update(uint64_t key, const std::string& payload);
  Status remove(uint64_t key);
  // Counts the live entries, used by the catalog statistics.
  Status count(uint64_t* count);

  // Sequential iteration over the live entries.  The visitor returns false to
  // stop the scan early.
  using ScanCallback = std::function<bool(uint64_t key, const std::string& payload)>;
  Status scan(const ScanCallback& visitor);

 private:
  Status scan_page(uint64_t page_id, const ScanCallback& visitor, bool* stopped);
  Status find_leaf(uint64_t key, std::vector<uint64_t>* path, uint64_t* leaf);
  Status descend_leftmost(std::vector<uint64_t>* path, uint64_t* leaf);
  Status insert_into_parent(const std::vector<uint64_t>& path, uint64_t left_child,
                             uint64_t separator_key, uint64_t right_child);
  Status split_leaf(uint64_t leaf, uint64_t* right_leaf, uint64_t* separator);
  Status split_internal(uint64_t page, uint64_t* right_page, uint64_t* separator,
                        uint32_t split_at);
  Status insert_pair_into_page(uint64_t page_id, uint32_t position,
                                uint64_t left_child, uint64_t separator_key,
                                uint64_t right_child);
  Status leaf_insert(uint64_t leaf, uint64_t key, const std::string& payload,
                     bool replace, bool allow_overflow);
  Status write_payload(uint64_t leaf, uint32_t slot_index, const std::string& payload);
  int find_slot(const uint8_t* page, uint64_t key);
  Status read_payload(const uint8_t* page, uint32_t slot_index, std::string* payload);

  Storage* storage_;
  uint64_t root_page_ = 0;
  // Number of scans that are currently running over the tree.  A scan may not
  // see the rows of a page move to another slot.
  int scan_depth_ = 0;
};

}  // namespace mvdb
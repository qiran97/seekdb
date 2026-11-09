#include "engine.h"
#include "value.h"

#include <algorithm>
#include <cstring>

namespace mvdb {

namespace {

// --- small binary serialisation helpers ----------------------------------
class BufWriter {
 public:
  void u8(uint8_t value) { buffer_.push_back(static_cast<char>(value)); }
  void u32(uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      buffer_.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
  }
  void u64(uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      buffer_.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
  }
  void i64(int64_t value) { u64(static_cast<uint64_t>(value)); }
  void str(const std::string& value) {
    u32(static_cast<uint32_t>(value.size()));
    buffer_.append(value);
  }
  const std::string& data() const { return buffer_; }
  std::string&& take() { return std::move(buffer_); }

 private:
  std::string buffer_;
};

class BufReader {
 public:
  BufReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool u8(uint8_t* value) {
    if (pos_ + 1 > size_) return fail();
    *value = static_cast<uint8_t>(data_[pos_++]);
    return true;
  }
  bool u32(uint32_t* value) {
    if (pos_ + 4 > size_) return fail();
    *value = 0;
    for (int i = 0; i < 4; ++i) {
      *value |= static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
    }
    pos_ += 4;
    return true;
  }
  bool u64(uint64_t* value) {
    if (pos_ + 8 > size_) return fail();
    *value = 0;
    for (int i = 0; i < 8; ++i) {
      *value |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
    }
    pos_ += 8;
    return true;
  }
  bool i64(int64_t* value) {
    uint64_t raw = 0;
    if (!u64(&raw)) return false;
    *value = static_cast<int64_t>(raw);
    return true;
  }
  bool str(std::string* value) {
    uint32_t length = 0;
    if (!u32(&length)) return false;
    if (pos_ + length > size_) return fail();
    value->assign(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return true;
  }
  bool ok() const { return ok_; }

 private:
  bool fail() {
    ok_ = false;
    return false;
  }
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

constexpr uint32_t kCatalogNextPageOffset = 8;
constexpr uint32_t kCatalogPayloadLengthOffset = 16;

}  // namespace

Engine::Engine() = default;

Engine::~Engine() { storage_.close(); }

Status Engine::open(const std::string& data_dir, size_t buffer_pool_bytes,
                    size_t cache_budget_bytes) {
  cache_budget_ = cache_budget_bytes;
  Status status = storage_.open(data_dir, buffer_pool_bytes);
  if (!status.ok()) return status;
  storage_.cache()->set_before_flush([this]() { return storage_.flush_undo_log(); });

  status = recover();
  if (!status.ok()) return status;
  status = load_catalog();
  if (!status.ok()) return status;
  return refresh_caches();
}

Status Engine::close() {
  Status status = storage_.close();
  return status;
}

Status Engine::recover() {
  std::vector<UndoRecord> records;
  Status status = storage_.undo_log()->records_from(0, &records);
  if (!status.ok()) return status;
  bool committed = false;
  for (const UndoRecord& record : records) {
    if (record.type == UndoLog::kRecordCommit) committed = true;
  }
  if (!committed) {
    // The last transaction did not reach its commit record: restore every
    // page to the image it had before the transaction started.
    for (size_t i = records.size(); i-- > 0;) {
      if (records[i].type != UndoLog::kRecordPageUndo) continue;
      if (records[i].payload.size() != kPageSize) continue;
      status = storage_.write_page(records[i].page_id, records[i].payload.data());
      if (!status.ok()) return status;
      storage_.cache()->drop(records[i].page_id);
    }
    log_info("recovery: rolled back %zu uncommitted page images", records.size());
  } else {
    log_info("recovery: last transaction was committed");
  }
  status = storage_.undo_log()->reset();
  if (!status.ok()) return status;
  storage_.cache()->drop(0);
  return storage_.load_db_header();
}

Status Engine::load_catalog() {
  uint64_t page_id = storage_.catalog_page();
  if (page_id == 0) return Status::Ok();
  std::string payload;
  uint64_t next = page_id;
  while (next != 0) {
    uint8_t page[kPageSize];
    Status status = storage_.read_page(next, page);
    if (!status.ok()) return status;
    uint32_t length = load_u32(page + kCatalogPayloadLengthOffset);
    uint32_t capacity = kPageSize - kCatalogHeaderSize;
    uint32_t chunk = std::min(length, capacity);
    payload.append(reinterpret_cast<const char*>(page + kCatalogHeaderSize), chunk);
    next = load_u32(page + kCatalogNextPageOffset);
  }
  BufReader reader(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  uint32_t table_count = 0;
  if (!reader.u32(&table_count)) {
    return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
  }
  for (uint32_t i = 0; i < table_count; ++i) {
    std::unique_ptr<Table> table(new Table());
    uint32_t id = 0;
    if (!reader.u32(&id)) return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    table->id = id;
    if (!reader.str(&table->def.name)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    }
    uint32_t column_count = 0;
    if (!reader.u32(&column_count)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    }
    for (uint32_t c = 0; c < column_count; ++c) {
      ColumnDef column;
      uint32_t type = 0;
      uint32_t flags = 0;
      if (!reader.str(&column.name) || !reader.u32(&type) ||
          !reader.u32(&column.length) || !reader.u32(&column.dim) ||
          !reader.u32(&flags)) {
        return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
      }
      column.type = static_cast<ColumnType>(type);
      column.primary_key = (flags & 1) != 0;
      table->def.columns.push_back(column);
    }
    if (!reader.u64(&table->root_page) || !reader.u64(&table->row_count)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    }
    uint8_t has_index = 0;
    if (!reader.u8(&has_index)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    }
    table->has_vector_index = has_index != 0;
    if (!reader.str(&table->index_name)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    }
    int64_t index_column = -1;
    if (!reader.i64(&index_column)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "catalog is corrupted");
    }
    table->index_column = static_cast<int>(index_column);
    table->def.pk_index = table->def.column_index(table->def.columns.empty() ? "" : table->def.columns[0].name);
    // Recompute the primary key and vector column indexes.
    table->def.pk_index = -1;
    table->def.vector_index = -1;
    for (size_t c = 0; c < table->def.columns.size(); ++c) {
      if (table->def.columns[c].primary_key) table->def.pk_index = static_cast<int>(c);
      if (table->def.columns[c].type == ColumnType::kVector) {
        table->def.vector_index = static_cast<int>(c);
      }
    }
    table->tree = BTree(&storage_, table->root_page);
    if (table->id >= next_table_id_) next_table_id_ = table->id + 1;
    tables_[table->def.name] = std::move(table);
  }
  return Status::Ok();
}

Status Engine::store_catalog() {
  BufWriter writer;
  writer.u32(static_cast<uint32_t>(tables_.size()));
  for (const auto& entry : tables_) {
    const Table& table = *entry.second;
    writer.u32(table.id);
    writer.str(table.def.name);
    writer.u32(static_cast<uint32_t>(table.def.columns.size()));
    for (const ColumnDef& column : table.def.columns) {
      writer.str(column.name);
      writer.u32(static_cast<uint32_t>(column.type));
      writer.u32(column.length);
      writer.u32(column.dim);
      writer.u32(column.primary_key ? 1u : 0u);
    }
    writer.u64(table.root_page);
    writer.u64(table.row_count);
    writer.u8(table.has_vector_index ? 1 : 0);
    writer.str(table.index_name);
    writer.i64(table.index_column);
  }
  std::string payload = writer.take();

  const uint32_t capacity = kPageSize - kCatalogHeaderSize;
  size_t page_count = (payload.size() + capacity - 1) / capacity;
  if (page_count == 0) page_count = 1;
  std::vector<uint64_t> pages;
  for (size_t i = 0; i < page_count; ++i) {
    uint64_t page_id = 0;
    Status status = storage_.allocate_page(PageType::kCatalog, &page_id);
    if (!status.ok()) return status;
    pages.push_back(page_id);
  }
  for (size_t i = 0; i < pages.size(); ++i) {
    uint8_t* page = nullptr;
    Status status = storage_.fetch_page(pages[i], &page);
    if (!status.ok()) return status;
    status = storage_.note_modification(pages[i], page);
    if (!status.ok()) {
      storage_.unpin_page(pages[i]);
      return status;
    }
    memset(page, 0, kPageSize);
    store_u32(page + 0, PageType::kCatalog);
    store_u32(page + 4, static_cast<uint32_t>(pages[i]));
    store_u32(page + kCatalogNextPageOffset,
              i + 1 < pages.size() ? static_cast<uint32_t>(pages[i + 1]) : 0u);
    store_u32(page + kCatalogPayloadLengthOffset, static_cast<uint32_t>(payload.size()));
    size_t offset = i * capacity;
    size_t chunk = std::min<size_t>(capacity, payload.size() - offset);
    if (chunk > 0) {
      memcpy(page + kCatalogHeaderSize, payload.data() + offset, chunk);
    }
    storage_.mark_dirty(pages[i]);
    storage_.unpin_page(pages[i]);
  }
  uint64_t old_catalog = storage_.catalog_page();
  storage_.set_catalog_page(pages[0]);
  Status status = storage_.store_db_header();
  if (!status.ok()) return status;
  // The previous catalog pages become free.
  uint64_t next = old_catalog;
  while (next != 0) {
    uint8_t page[kPageSize];
    status = storage_.read_page(next, page);
    if (!status.ok()) break;
    uint64_t following = load_u32(page + kCatalogNextPageOffset);
    status = storage_.free_page(next);
    if (!status.ok()) break;
    next = following;
  }
  return Status::Ok();
}

Engine::Table* Engine::find_table(const std::string& name) {
  auto it = tables_.find(name);
  if (it == tables_.end()) return nullptr;
  return it->second.get();
}
// ---------------------------------------------------------------------------
// transactions
// ---------------------------------------------------------------------------

Status Engine::begin_transaction(uint64_t txn_id) {
  if (in_transaction_) {
    return Status::Error(ErrorCode::kTransactionError,
                         "a transaction is already active, nested transactions "
                         "are not supported");
  }
  in_transaction_ = true;
  current_txn_id_ = txn_id;
  storage_.begin_transaction(txn_id);
  cache_changes_.clear();
  return Status::Ok();
}

Status Engine::begin_statement(uint64_t* savepoint) {
  Status status = storage_.flush_undo_log();
  if (!status.ok()) return status;
  *savepoint = storage_.undo_log()->position();
  storage_.reset_logged_pages();
  cache_changes_.clear();
  return Status::Ok();
}

Status Engine::commit_transaction() {
  if (!in_transaction_) return Status::Ok();
  if (catalog_dirty_) {
    Status status = store_catalog();
    if (!status.ok()) return status;
    catalog_dirty_ = false;
  }
  if (!storage_.has_changes()) {
    // A read only statement does not have to touch the disk at all.
    storage_.end_transaction();
    current_txn_id_ = 0;
    in_transaction_ = false;
    cache_changes_.clear();
    return Status::Ok();
  }
  storage_.set_last_committed_txn(current_txn_id_);
  Status status = storage_.store_db_header();
  if (!status.ok()) return status;
  // The undo images must be on disk before the modified pages are written.
  status = storage_.flush_undo_log();
  if (!status.ok()) return status;
  status = storage_.cache()->flush_all();
  if (!status.ok()) return status;
  status = storage_.file()->sync();
  if (!status.ok()) return status;
  // The commit record is the commit point of the transaction.
  status = storage_.undo_log()->append_marker(UndoLog::kRecordCommit, current_txn_id_);
  if (!status.ok()) return status;
  status = storage_.undo_log()->sync();
  if (!status.ok()) return status;
  status = storage_.undo_log()->reset();
  if (!status.ok()) return status;
  storage_.end_transaction();
  current_txn_id_ = 0;
  in_transaction_ = false;
  cache_changes_.clear();
  return Status::Ok();
}

Status Engine::apply_undo(const UndoRecord& record) {
  if (record.type != UndoLog::kRecordPageUndo) return Status::Ok();
  if (record.payload.size() != kPageSize) return Status::Ok();
  Status status = storage_.write_page(record.page_id, record.payload.data());
  if (!status.ok()) return status;
  storage_.cache()->drop(record.page_id);
  return Status::Ok();
}

Status Engine::rollback_to(uint64_t savepoint) {
  std::vector<UndoRecord> records;
  Status status = storage_.undo_log()->records_from(savepoint, &records);
  if (!status.ok()) return status;
  for (size_t i = records.size(); i-- > 0;) {
    status = apply_undo(records[i]);
    if (!status.ok()) return status;
  }
  status = storage_.undo_log()->truncate_to(savepoint);
  if (!status.ok()) return status;
  // Restore the in-memory row cache.
  for (size_t i = cache_changes_.size(); i-- > 0;) {
    const CacheChange& change = cache_changes_[i];
    for (auto& entry : tables_) {
      if (entry.second->id != change.table_id) continue;
      Table* table = entry.second.get();
      if (!table->cache_enabled) break;
      if (change.existed) {
        table->cache[change.key] = change.old_row;
      } else {
        table->cache.erase(change.key);
      }
    }
  }
  cache_changes_.clear();
  return Status::Ok();
}

Status Engine::rollback_open_transaction() {
  if (!in_transaction_) return Status::Ok();
  Status status = rollback_to(0);
  if (status.ok()) {
    status = storage_.undo_log()->reset();
  }
  if (status.ok()) {
    storage_.end_transaction();
    current_txn_id_ = 0;
    in_transaction_ = false;
  }
  return status;
}

// ---------------------------------------------------------------------------
// schema management
// ---------------------------------------------------------------------------

Status Engine::create_table(const Statement& statement) {
  const std::string& name = statement.table;
  if (find_table(name) != nullptr) {
    if (statement.if_not_exists) return Status::Ok();
    return Status::Error(ErrorCode::kTableExists, "table '" + name + "' already exists");
  }
  TableDef def = statement.table_def;
  def.name = name;
  def.pk_index = -1;
  def.vector_index = -1;
  std::vector<std::string> seen;
  for (size_t i = 0; i < def.columns.size(); ++i) {
    ColumnDef& column = def.columns[i];
    for (const std::string& other : seen) {
      if (iequals(other, column.name)) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "duplicate column name '" + column.name + "'");
      }
    }
    seen.push_back(column.name);
    if (column.primary_key) {
      if (column.type != ColumnType::kBigInt) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "the primary key must be a BIGINT column");
      }
      if (def.pk_index >= 0) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "a table can only have one primary key");
      }
      def.pk_index = static_cast<int>(i);
    }
    if (column.type == ColumnType::kVector) {
      if (def.vector_index >= 0) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "a table can only have one vector column");
      }
      def.vector_index = static_cast<int>(i);
    }
  }
  if (def.pk_index < 0) {
    return Status::Error(ErrorCode::kSyntaxError,
                         "table '" + name + "' needs a BIGINT PRIMARY KEY");
  }

  std::unique_ptr<Table> table(new Table());
  table->id = next_table_id_++;
  table->def = def;
  table->root_page = 0;
  table->row_count = 0;
  table->tree = BTree(&storage_, 0);
  tables_[name] = std::move(table);
  catalog_dirty_ = true;
  log_info("created table %s (id %u)", name.c_str(), tables_[name]->id);
  return Status::Ok();
}

Status Engine::drop_table(const Statement& statement) {
  Table* table = find_table(statement.table);
  if (table == nullptr) {
    if (statement.if_exists) return Status::Ok();
    return Status::Error(ErrorCode::kUnknownTable,
                         "table '" + statement.table + "' does not exist");
  }
  tables_.erase(statement.table);
  catalog_dirty_ = true;
  log_info("dropped table %s", statement.table.c_str());
  return Status::Ok();
}

Status Engine::create_vector_index(const Statement& statement) {
  Table* table = find_table(statement.table);
  if (table == nullptr) {
    return Status::Error(ErrorCode::kUnknownTable,
                         "table '" + statement.table + "' does not exist");
  }
  int column = table->def.column_index(statement.index_column);
  if (column < 0) {
    return Status::Error(ErrorCode::kUnknownColumn,
                         "unknown column '" + statement.index_column + "'");
  }
  if (column != table->def.vector_index) {
    return Status::Error(ErrorCode::kSyntaxError,
                         "a vector index can only be created on the vector column");
  }
  if (table->has_vector_index) {
    return Status::Error(ErrorCode::kTableExists,
                         "table '" + statement.table + "' already has a vector index");
  }
  table->has_vector_index = true;
  table->index_name = statement.index_name;
  table->index_column = column;
  catalog_dirty_ = true;
  log_info("created vector index %s on %s(%s)", statement.index_name.c_str(),
           statement.table.c_str(), statement.index_column.c_str());
  return Status::Ok();
}

Status Engine::drop_vector_index(const Statement& statement) {
  bool found = false;
  for (auto& entry : tables_) {
    Table* table = entry.second.get();
    if (table->has_vector_index && iequals(table->index_name, statement.index_name)) {
      table->has_vector_index = false;
      table->index_name.clear();
      table->index_column = -1;
      catalog_dirty_ = true;
      found = true;
    }
  }
  if (!found) {
    return Status::Error(ErrorCode::kUnknownTable,
                         "vector index '" + statement.index_name + "' does not exist");
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// row encoding
// ---------------------------------------------------------------------------

Status Engine::encode_row(const Table& table, const RowValues& row,
                           std::string* out) const {
  BufWriter writer;
  std::vector<int> value_columns = table.def.value_columns();
  writer.u32(static_cast<uint32_t>(value_columns.size()));
  for (int index : value_columns) {
    int position = value_position(table.def, index);
    const std::string& value = (position >= 0 && position < static_cast<int>(row.strings.size()))
                                   ? row.strings[position]
                                   : std::string();
    writer.str(value);
  }
  uint32_t dim = table.def.vector_index >= 0
                     ? table.def.columns[table.def.vector_index].dim
                     : 0;
  writer.u32(dim);
  if (dim > 0) {
    for (uint32_t i = 0; i < dim; ++i) {
      float value = i < row.vector.size() ? row.vector[i] : 0.0f;
      uint32_t bits = 0;
      memcpy(&bits, &value, sizeof(bits));
      writer.u32(bits);
    }
  }
  *out = writer.take();
  return Status::Ok();
}

Status Engine::decode_row(const Table& table, const std::string& payload,
                           RowValues* out) const {
  (void)table;
  BufReader reader(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  uint32_t value_count = 0;
  if (!reader.u32(&value_count)) {
    return Status::Error(ErrorCode::kStorageCorrupt, "row payload is corrupted");
  }
  out->strings.clear();
  for (uint32_t i = 0; i < value_count; ++i) {
    std::string value;
    if (!reader.str(&value)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "row payload is corrupted");
    }
    out->strings.push_back(std::move(value));
  }
  uint32_t dim = 0;
  if (!reader.u32(&dim)) {
    return Status::Error(ErrorCode::kStorageCorrupt, "row payload is corrupted");
  }
  out->vector.clear();
  for (uint32_t i = 0; i < dim; ++i) {
    uint32_t bits = 0;
    if (!reader.u32(&bits)) {
      return Status::Error(ErrorCode::kStorageCorrupt, "row payload is corrupted");
    }
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    out->vector.push_back(value);
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// row mutation
// ---------------------------------------------------------------------------

void Engine::cache_insert(Table* table, const RowValues& row) {
  if (!table->cache_enabled) {
    table->cache_stale = true;
    return;
  }
  size_t bytes = 64;
  for (const std::string& value : row.strings) bytes += value.size() + 16;
  bytes += row.vector.size() * sizeof(float);
  table->cache[row.key] = row;
  table->cache_bytes += bytes;
  if (table->cache_bytes > cache_budget_ && cache_budget_ > 0) {
    log_info("table %s exceeds the cache budget, falling back to disk scans",
             table->def.name.c_str());
    drop_cache(table);
  }
}

void Engine::cache_update(Table* table, int64_t key, const RowValues& row) {
  if (!table->cache_enabled) {
    table->cache_stale = true;
    return;
  }
  auto it = table->cache.find(key);
  if (it == table->cache.end()) {
    cache_insert(table, row);
    return;
  }
  it->second = row;
}

void Engine::cache_remove(Table* table, int64_t key) {
  if (!table->cache_enabled) {
    table->cache_stale = true;
    return;
  }
  table->cache.erase(key);
}

void Engine::drop_cache(Table* table) {
  table->cache_enabled = false;
  table->cache.clear();
  table->cache_bytes = 0;
}

Status Engine::insert_row(Table* table, const RowValues& row) {
  std::string payload;
  Status status = encode_row(*table, row, &payload);
  if (!status.ok()) return status;
  status = table->tree.insert(static_cast<uint64_t>(row.key), payload);
  if (!status.ok()) return status;
  ++table->row_count;
  if (table->root_page != table->tree.root_page()) {
    table->root_page = table->tree.root_page();
    // The root of the tree moved, the catalog has to record the new page.
    catalog_dirty_ = true;
  }
  if (table->cache_enabled) {
    CacheChange change;
    change.table_id = table->id;
    change.key = row.key;
    change.existed = false;
    cache_changes_.push_back(change);
  }
  cache_insert(table, row);
  return Status::Ok();
}

Status Engine::update_row(Table* table, int64_t key, const RowValues& row) {
  std::string payload;
  Status status = encode_row(*table, row, &payload);
  if (!status.ok()) return status;
  status = table->tree.update(static_cast<uint64_t>(key), payload);
  if (!status.ok()) return status;
  if (table->root_page != table->tree.root_page()) {
    table->root_page = table->tree.root_page();
    catalog_dirty_ = true;
  }
  if (table->cache_enabled) {
    CacheChange change;
    change.table_id = table->id;
    change.key = key;
    auto it = table->cache.find(key);
    change.existed = it != table->cache.end();
    if (change.existed) change.old_row = it->second;
    cache_changes_.push_back(change);
  }
  cache_update(table, key, row);
  return Status::Ok();
}

Status Engine::delete_row(Table* table, int64_t key) {
  Status status = table->tree.remove(static_cast<uint64_t>(key));
  if (!status.ok()) return status;
  if (table->row_count > 0) --table->row_count;
  catalog_dirty_ = true;
  if (table->cache_enabled) {
    CacheChange change;
    change.table_id = table->id;
    change.key = key;
    auto it = table->cache.find(key);
    change.existed = it != table->cache.end();
    if (change.existed) change.old_row = it->second;
    cache_changes_.push_back(change);
  }
  cache_remove(table, key);
  return Status::Ok();
}

Status Engine::build_cache_if_possible(Table* table) {
  if (table->cache_enabled) return Status::Ok();
  if (!table->cache_stale) return Status::Ok();
  table->cache_stale = false;
  size_t per_row = 64;
  for (const ColumnDef& column : table->def.columns) {
    if (column.type == ColumnType::kVarchar) per_row += column.length;
    if (column.type == ColumnType::kVector) per_row += column.dim * sizeof(float);
  }
  uint64_t estimate = static_cast<uint64_t>(per_row) * table->row_count;
  if (cache_budget_ == 0 || estimate > cache_budget_) {
    log_info("table %s stays on disk (%llu bytes estimated)",
             table->def.name.c_str(), static_cast<unsigned long long>(estimate));
    return Status::Ok();
  }
  table->cache.clear();
  table->cache_bytes = 0;
  Status status = scan_rows(table, [table](int64_t key, const RowValues& row) {
    table->cache[key] = row;
    size_t bytes = 64;
    for (const std::string& value : row.strings) bytes += value.size() + 16;
    bytes += row.vector.size() * sizeof(float);
    table->cache_bytes += bytes;
    return true;
  });
  if (!status.ok()) {
    drop_cache(table);
    return status;
  }
  table->cache_enabled = true;
  log_info("table %s cached in memory (%zu bytes)", table->def.name.c_str(),
           table->cache_bytes);
  return Status::Ok();
}

Status Engine::scan_rows(Table* table, const RowVisitor& visitor) {
  if (!table->cache_enabled && table->cache_stale) {
    Status status = build_cache_if_possible(table);
    if (!status.ok()) return status;
  }
  if (table->cache_enabled) {
    for (const auto& entry : table->cache) {
      if (!visitor(entry.first, entry.second)) return Status::Ok();
    }
    return Status::Ok();
  }
  std::string payload;
  Status status = table->tree.scan([&](uint64_t key, const std::string& data) {
    RowValues row;
    row.key = static_cast<int64_t>(key);
    Status decode_status = decode_row(*table, data, &row);
    if (!decode_status.ok()) return false;
    return visitor(row.key, row);
  });
  return status;
}

Status Engine::refresh_caches() {
  for (auto& entry : tables_) {
    Table* table = entry.second.get();
    if (table->row_count == 0) continue;
    // Estimate the memory footprint; the cache is only used when the whole
    // table fits into the budget.
    size_t per_row = 64;
    for (const ColumnDef& column : table->def.columns) {
      if (column.type == ColumnType::kVarchar) per_row += column.length;
      if (column.type == ColumnType::kVector) per_row += column.dim * sizeof(float);
    }
    uint64_t estimate = static_cast<uint64_t>(per_row) * table->row_count;
    if (cache_budget_ == 0 || estimate > cache_budget_) {
      log_info("table %s stays on disk (%llu bytes estimated)", table->def.name.c_str(),
               static_cast<unsigned long long>(estimate));
      continue;
    }
    // The cache is filled from the clustered index first, it is only used
    // for queries once every row has been loaded.
    table->cache_enabled = false;
    table->cache.clear();
    table->cache_bytes = 0;
    Status status = scan_rows(table, [table](int64_t key, const RowValues& row) {
      table->cache[key] = row;
      size_t bytes = 64;
      for (const std::string& value : row.strings) bytes += value.size() + 16;
      bytes += row.vector.size() * sizeof(float);
      table->cache_bytes += bytes;
      return true;
    });
    if (!status.ok()) {
      drop_cache(table);
      return status;
    }
    table->cache_enabled = true;
    log_info("table %s cached in memory (%zu bytes)", table->def.name.c_str(),
             table->cache_bytes);
  }
  return Status::Ok();
}
// ---------------------------------------------------------------------------
// statement dispatch
// ---------------------------------------------------------------------------

Status Engine::execute(const Statement& statement, ResultSet* result) {
  result->has_result_set = false;
  result->columns.clear();
  result->rows.clear();
  result->affected_rows = 0;

  switch (statement.type) {
    case StatementType::kBegin: {
      if (in_transaction_) {
        return Status::Error(ErrorCode::kTransactionError,
                             "a transaction is already active");
      }
      Status status = begin_transaction(next_txn_id_++);
      if (status.ok()) {
        txn_start_position_ = storage_.undo_log()->position();
      }
      return status;
    }
    case StatementType::kCommit: {
      if (!in_transaction_) return Status::Ok();
      return commit_transaction();
    }
    case StatementType::kRollback: {
      if (!in_transaction_) return Status::Ok();
      Status status = rollback_to(txn_start_position_);
      if (!status.ok()) return status;
      status = storage_.undo_log()->reset();
      if (!status.ok()) return status;
      storage_.end_transaction();
      current_txn_id_ = 0;
      in_transaction_ = false;
      cache_changes_.clear();
      return Status::Ok();
    }
    case StatementType::kSet: {
      std::string name = to_lower(statement.name);
      std::string value = to_lower(trim(statement.value));
      if (name == "search_mode" || name == "vector_search_mode" ||
          name == "vector_search" || name == "searchmode" ||
          name == "ann_mode" || name == "approximate") {
        if (value == "ann" || value == "approximate" || value == "1" ||
            value == "on" || value == "true") {
          search_mode_ann_ = true;
        } else if (value == "exact" || value == "0" || value == "off" ||
                   value == "false") {
          search_mode_ann_ = false;
        } else {
          return Status::Error(ErrorCode::kSyntaxError,
                               "unknown search mode '" + statement.value + "'");
        }
        return Status::Ok();
      }
      if (name == "autocommit" || name == "names" || name == "character_set" ||
          name == "character_set_results" || name == "character_set_client" ||
          name == "sql_mode" || name == "time_zone" || name == "session" ||
          name == "transaction_isolation" || name == "wait_timeout" ||
          name == "net_write_timeout" || name == "max_allowed_packet") {
        return Status::Ok();
      }
      return Status::Error(ErrorCode::kSyntaxError,
                           "unknown system variable '" + statement.name + "'");
    }
    case StatementType::kSetSearchMode: {
      std::string value = to_lower(trim(statement.value));
      if (value == "ann" || value == "approximate") {
        search_mode_ann_ = true;
      } else if (value == "exact") {
        search_mode_ann_ = false;
      } else {
        return Status::Error(ErrorCode::kSyntaxError,
                             "unknown search mode '" + statement.value + "'");
      }
      return Status::Ok();
    }
    case StatementType::kShowTables: {
      result->has_result_set = true;
      result->columns.push_back("Tables_in_minivectordb");
      std::vector<std::string> names;
      for (const auto& entry : tables_) names.push_back(entry.first);
      std::sort(names.begin(), names.end());
      for (const std::string& name : names) {
        std::vector<ResultCell> row;
        ResultCell cell;
        cell.type = ResultCell::Type::kText;
        cell.text = name;
        row.push_back(cell);
        result->rows.push_back(std::move(row));
      }
      return Status::Ok();
    }
    case StatementType::kShutdown: {
      shutdown_requested_ = true;
      return Status::Ok();
    }
    default:
      break;
  }

  const bool explicit_transaction = in_transaction_;
  if (!explicit_transaction) {
    Status status = begin_transaction(next_txn_id_++);
    if (!status.ok()) return status;
    txn_start_position_ = storage_.undo_log()->position();
  }
  uint64_t savepoint = 0;
  Status status = begin_statement(&savepoint);
  if (!status.ok()) return status;
  status = dispatch(statement, result);
  if (!status.ok()) {
    Status undo_status = rollback_to(savepoint);
    if (!undo_status.ok()) {
      log_error("rollback after a failed statement failed: %s",
                undo_status.message().c_str());
    }
    if (!explicit_transaction) {
      storage_.end_transaction();
      current_txn_id_ = 0;
      in_transaction_ = false;
    }
    return status;
  }
  if (!explicit_transaction) {
    status = commit_transaction();
    if (!status.ok()) return status;
  }
  return status;
}

Status Engine::dispatch(const Statement& statement, ResultSet* result) {
  switch (statement.type) {
    case StatementType::kCreateTable: return create_table(statement);
    case StatementType::kDropTable: return drop_table(statement);
    case StatementType::kCreateVectorIndex: return create_vector_index(statement);
    case StatementType::kDropVectorIndex: return drop_vector_index(statement);
    case StatementType::kInsert: return execute_insert(statement, result);
    case StatementType::kUpdate: return execute_update(statement, result);
    case StatementType::kDelete: return execute_delete(statement, result);
    case StatementType::kSelect: return execute_select(statement, result);
    default: break;
  }
  return Status::Error(ErrorCode::kUnsupportedYet, "unsupported statement");
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

Status Engine::execute_insert(const Statement& statement, ResultSet* result) {
  Table* table = find_table(statement.table);
  if (table == nullptr) {
    return Status::Error(ErrorCode::kUnknownTable,
                         "table '" + statement.table + "' does not exist");
  }
  std::vector<int> value_columns = table->def.value_columns();
  std::vector<int> targets;
  if (statement.insert_columns.empty()) {
    for (size_t i = 0; i < table->def.columns.size(); ++i) {
      targets.push_back(static_cast<int>(i));
    }
  } else {
    for (const std::string& name : statement.insert_columns) {
      int index = table->def.column_index(name);
      if (index < 0) {
        return Status::Error(ErrorCode::kUnknownColumn,
                             "unknown column '" + name + "' in table '" +
                                 statement.table + "'");
      }
      targets.push_back(index);
    }
  }

  for (const auto& row : statement.insert_rows) {
    if (row.size() != targets.size()) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "column count does not match value count");
    }
    RowValues values;
    values.strings.assign(value_columns.size(), std::string());
    int64_t key = 0;
    bool key_set = false;
    for (size_t i = 0; i < row.size(); ++i) {
      int column_index = targets[i];
      const ColumnDef& column = table->def.columns[column_index];
      const Expr& expr = *row[i];
      if (column_index == table->def.pk_index) {
        if (expr.type != ExprType::kIntLiteral) {
          return Status::Error(ErrorCode::kSyntaxError,
                               "the primary key must be an integer literal");
        }
        key = expr.int_value;
        key_set = true;
        continue;
      }
      if (column.type == ColumnType::kVarchar) {
        if (expr.type != ExprType::kStringLiteral) {
          return Status::Error(ErrorCode::kSyntaxError,
                               "column '" + column.name + "' expects a string value");
        }
        if (utf8_length(expr.str_value) > column.length) {
          return Status::Error(ErrorCode::kDataTooLong,
                               "value is too long for column '" + column.name + "'");
        }
        int position = value_position(table->def, column_index);
        if (position < 0 || position >= static_cast<int>(values.strings.size())) {
          return Status::Error(ErrorCode::kInternalError, "column mapping is broken");
        }
        values.strings[position] = expr.str_value;
        continue;
      }
      if (column.type == ColumnType::kVector) {
        if (expr.type != ExprType::kStringLiteral) {
          return Status::Error(ErrorCode::kSyntaxError,
                               "column '" + column.name + "' expects a vector literal");
        }
        VectorValue parsed;
        Status status = parse_vector_literal(expr.str_value, column.dim, &parsed);
        if (!status.ok()) return status;
        values.vector = parsed.values;
        continue;
      }
      return Status::Error(ErrorCode::kSyntaxError,
                           "unsupported value for column '" + column.name + "'");
    }
    if (!key_set) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "the primary key column must be provided");
    }
    values.key = key;
    Status status = insert_row(table, values);
    if (!status.ok()) return status;
    ++result->affected_rows;
    result->last_insert_id = key;
    result->last_insert_id_set = true;
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

Status Engine::execute_update(const Statement& statement, ResultSet* result) {
  Table* table = find_table(statement.table);
  if (table == nullptr) {
    return Status::Error(ErrorCode::kUnknownTable,
                         "table '" + statement.table + "' does not exist");
  }
  std::vector<int> value_columns = table->def.value_columns();
  struct ResolvedAssignment {
    int column = -1;
    size_t value_position = 0;
    bool is_vector = false;
    int64_t number = 0;
    std::string text;
    std::vector<float> vector;
  };
  std::vector<ResolvedAssignment> assignments;
  for (const Assignment& assignment : statement.assignments) {
    int index = table->def.column_index(assignment.column);
    if (index < 0) {
      return Status::Error(ErrorCode::kUnknownColumn,
                           "unknown column '" + assignment.column + "' in table '" +
                               statement.table + "'");
    }
    if (index == table->def.pk_index) {
      return Status::Error(ErrorCode::kUnsupportedYet,
                           "updating the primary key is not supported");
    }
    ResolvedAssignment resolved;
    resolved.column = index;
    resolved.value_position = static_cast<size_t>(value_position(table->def, index));
    const ColumnDef& column = table->def.columns[index];
    if (column.type == ColumnType::kVarchar) {
      if (assignment.value->type != ExprType::kStringLiteral) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "column '" + column.name + "' expects a string value");
      }
      if (utf8_length(assignment.value->str_value) > column.length) {
        return Status::Error(ErrorCode::kDataTooLong,
                             "value is too long for column '" + column.name + "'");
      }
      resolved.text = assignment.value->str_value;
    } else if (column.type == ColumnType::kVector) {
      if (assignment.value->type != ExprType::kStringLiteral) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "column '" + column.name + "' expects a vector literal");
      }
      VectorValue parsed;
      Status status = parse_vector_literal(assignment.value->str_value, column.dim,
                                           &parsed);
      if (!status.ok()) return status;
      resolved.is_vector = true;
      resolved.vector = parsed.values;
    } else {
      return Status::Error(ErrorCode::kSyntaxError,
                           "unsupported assignment for column '" + column.name + "'");
    }
    assignments.push_back(std::move(resolved));
  }

  ResolvedCondition filter;
  bool has_filter = false;
  if (statement.where != nullptr) {
    Status resolve_status = resolve_condition(*statement.where, table->def, &filter);
    if (!resolve_status.ok()) return resolve_status;
    has_filter = true;
  }

  uint64_t affected = 0;
  Status status = scan_rows(table, [&](int64_t key, const RowValues& row) {
    if (has_filter && !evaluate_condition(filter, table->def, row)) return true;
    RowValues updated = row;
    for (const ResolvedAssignment& assignment : assignments) {
      if (assignment.is_vector) {
        updated.vector = assignment.vector;
      } else {
        updated.strings[assignment.value_position] = assignment.text;
      }
    }
    Status update_status = update_row(table, key, updated);
    if (!update_status.ok()) return false;
    ++affected;
    return true;
  });
  if (!status.ok()) return status;
  result->affected_rows = affected;
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

Status Engine::execute_delete(const Statement& statement, ResultSet* result) {
  Table* table = find_table(statement.table);
  if (table == nullptr) {
    return Status::Error(ErrorCode::kUnknownTable,
                         "table '" + statement.table + "' does not exist");
  }
  ResolvedCondition filter;
  bool has_filter = false;
  if (statement.where != nullptr) {
    Status resolve_status = resolve_condition(*statement.where, table->def, &filter);
    if (!resolve_status.ok()) return resolve_status;
    has_filter = true;
  }
  std::vector<int64_t> victims;
  Status status = scan_rows(table, [&](int64_t key, const RowValues& row) {
    if (has_filter && !evaluate_condition(filter, table->def, row)) return true;
    victims.push_back(key);
    return true;
  });
  if (!status.ok()) return status;
  for (int64_t key : victims) {
    status = delete_row(table, key);
    if (!status.ok()) return status;
    ++result->affected_rows;
  }
  return Status::Ok();
}

}  // namespace mvdb
// Query engine of MiniVectorDB: catalog, transactions and statement execution.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base.h"
#include "sql.h"
#include "storage.h"

namespace mvdb {

// A single result value.  The executor produces typed cells, the protocol
// layer renders them.
struct ResultCell {
  enum class Type { kInteger, kText, kVector, kReal };
  Type type = Type::kText;
  int64_t number = 0;
  double real = 0.0;
  std::string text;
  std::vector<float> vector;
};

struct ResultSet {
  bool has_result_set = false;
  std::vector<std::string> columns;
  std::vector<std::vector<ResultCell>> rows;
  uint64_t affected_rows = 0;
  bool last_insert_id_set = false;
  int64_t last_insert_id = 0;
};

// The row payload as stored in the clustered index.
struct RowValues {
  int64_t key = 0;
  std::vector<std::string> strings;  // the VARCHAR columns in schema order
  std::vector<float> vector;         // the vector column, empty when absent
};
// A resolved operand of a comparison.
struct ScalarOperand {
  enum class Kind { kColumn, kInteger, kText };
  Kind kind = Kind::kInteger;
  int column = -1;         // the column index in the table definition
  ColumnType column_type = ColumnType::kBigInt;
  int64_t number = 0;
  std::string text;
};

// A condition that has been resolved against a table definition.
struct ResolvedCondition {
  ConditionType type = ConditionType::kCompare;
  CompareOp op = CompareOp::kEq;
  ScalarOperand left;
  ScalarOperand right;
  std::vector<ResolvedCondition> children;
};

// The position of a column inside the vector of value columns.
int value_position(const TableDef& def, int column_index);

// Resolves a parsed condition against a table definition.
Status resolve_condition(const Condition& condition, const TableDef& def,
                          ResolvedCondition* out);

// Evaluates a resolved condition against one row.
bool evaluate_condition(const ResolvedCondition& condition, const TableDef& def,
                        const RowValues& row);



class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Status open(const std::string& data_dir, size_t buffer_pool_bytes,
              size_t cache_budget_bytes);
  Status close();

  // Executes one statement and fills the result set.
  Status execute(const Statement& statement, ResultSet* result);

  bool in_transaction() const { return in_transaction_; }
  bool shutdown_requested() const { return shutdown_requested_; }
  bool search_mode_ann() const { return search_mode_ann_; }

  // Rolls back an open transaction, used on shutdown.
  Status rollback_open_transaction();

 private:
  struct Table {
    uint32_t id = 0;
    TableDef def;
    uint64_t root_page = 0;
    uint64_t row_count = 0;
    bool has_vector_index = false;
    std::string index_name;
    int index_column = -1;
    BTree tree{nullptr, 0};

    // Optional complete in-memory mirror of the table.  It is only used when
    // the whole table comfortably fits into the memory budget; otherwise the
    // engine falls back to reading the clustered index.
    bool cache_enabled = false;
    bool cache_stale = false;   // the table changed while the cache was off
    std::map<int64_t, RowValues> cache;
    size_t cache_bytes = 0;
  };

  // --- catalog ---------------------------------------------------------
  Status load_catalog();
  Status store_catalog();
  Table* find_table(const std::string& name);
  Status create_table(const Statement& statement);
  Status drop_table(const Statement& statement);
  Status create_vector_index(const Statement& statement);
  Status drop_vector_index(const Statement& statement);

  // The state of the tables at the beginning of the running statement and at
  // the beginning of the running transaction.
  struct TableState {
    uint64_t root_page = 0;
    uint64_t row_count = 0;
  };
  std::map<std::string, TableState> statement_state_;
  std::map<std::string, TableState> transaction_state_;

  // --- transactions ----------------------------------------------------
  Status begin_transaction(uint64_t txn_id);
  Status commit_transaction();
  Status rollback_to(uint64_t savepoint,
                     const std::map<std::string, TableState>& state);
  Status begin_statement(uint64_t* savepoint);

  // --- statement dispatch ----------------------------------------------
  Status dispatch(const Statement& statement, ResultSet* result);
  Status execute_insert(const Statement& statement, ResultSet* result);
  Status execute_update(const Statement& statement, ResultSet* result);
  Status execute_delete(const Statement& statement, ResultSet* result);
  Status execute_select(const Statement& statement, ResultSet* result);

  // --- helpers ---------------------------------------------------------
  Status recover();
  Status refresh_caches();
  // Builds the in-memory mirror of a table when the table fits into the
  // memory budget.
  Status build_cache_if_possible(Table* table);
  using RowVisitor = std::function<bool(int64_t key, const RowValues& row)>;
  Status scan_rows(Table* table, const RowVisitor& visitor);
  // Reads a single row of a table.
  Status read_row(Table* table, int64_t key, RowValues* row);
  Status apply_undo(const UndoRecord& record);
  Status encode_row(const Table& table, const RowValues& row, std::string* out) const;
  Status decode_row(const Table& table, const std::string& payload, RowValues* out) const;
  Status insert_row(Table* table, const RowValues& row);
  Status update_row(Table* table, int64_t key, const RowValues& row);
  Status delete_row(Table* table, int64_t key);
  Status refresh_cache(Table* table);
  void drop_cache(Table* table);
  void cache_insert(Table* table, const RowValues& row);
  void cache_update(Table* table, int64_t key, const RowValues& row);
  void cache_remove(Table* table, int64_t key);
  size_t cache_budget_ = 0;
  uint64_t txn_start_position_ = 0;

  Storage storage_;
  std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
  uint32_t next_table_id_ = 1;
  uint64_t next_txn_id_ = 1;
  uint64_t current_txn_id_ = 0;
  bool in_transaction_ = false;
  bool catalog_dirty_ = false;
  bool search_mode_ann_ = false;
  bool shutdown_requested_ = false;
  bool cache_disabled_ = false;
};

}  // namespace mvdb
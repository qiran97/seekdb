// SQL AST and parser for the MiniVectorDB statement subset.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base.h"

namespace mvdb {

enum class ColumnType { kBigInt, kVarchar, kVector };

struct ColumnDef {
  std::string name;
  ColumnType type = ColumnType::kBigInt;
  uint32_t length = 0;   // VARCHAR capacity in unicode characters
  uint32_t dim = 0;      // VECTOR dimension
  bool primary_key = false;
};

struct TableDef {
  std::string name;
  std::vector<ColumnDef> columns;
  int pk_index = -1;
  int vector_index = -1;

  int column_index(const std::string& column_name) const;
  // Column indexes of the payload columns (everything except the primary key
  // and the vector column).
  std::vector<int> value_columns() const;
};

enum class ExprType {
  kColumn,
  kIntLiteral,
  kStringLiteral,
  kVectorLiteral,
  kL2Distance,
};

struct Expr {
  ExprType type = ExprType::kIntLiteral;
  std::string name;       // column name for kColumn
  std::string alias;      // output alias
  int64_t int_value = 0;
  std::string str_value;  // string literal / raw vector literal text
  std::vector<float> vector_value;
  int column_ref = -1;    // resolved index into TableDef::columns
  std::vector<std::unique_ptr<Expr>> children;
};

enum class CompareOp { kEq, kNe, kLt, kLe, kGt, kGe };

enum class ConditionType { kCompare, kAnd, kOr, kNot };

struct Condition {
  ConditionType type = ConditionType::kCompare;
  CompareOp op = CompareOp::kEq;
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;
  std::vector<std::unique_ptr<Condition>> children;
};

struct OrderByItem {
  std::unique_ptr<Expr> expr;
  bool ascending = true;
  int ordinal = -1;  // 1 based positional reference
};

enum class StatementType {
  kCreateTable,
  kDropTable,
  kCreateVectorIndex,
  kDropVectorIndex,
  kInsert,
  kSelect,
  kUpdate,
  kDelete,
  kBegin,
  kCommit,
  kRollback,
  kSet,
  kShowTables,
  kSetSearchMode,
  kShutdown,
  kEmpty,
};

struct Assignment {
  std::string column;
  std::unique_ptr<Expr> value;
};

struct Statement {
  StatementType type = StatementType::kEmpty;

  // CREATE / DROP
  std::string table;
  TableDef table_def;
  bool if_not_exists = false;
  bool if_exists = false;
  std::string index_name;
  std::string index_column;

  // INSERT
  std::vector<std::string> insert_columns;
  std::vector<std::vector<std::unique_ptr<Expr>>> insert_rows;

  // SELECT
  std::vector<std::unique_ptr<Expr>> select_list;
  bool select_star = false;
  std::unique_ptr<Condition> where;
  std::vector<OrderByItem> order_by;
  int64_t limit = -1;
  int64_t offset = 0;

  // UPDATE
  std::vector<Assignment> assignments;

  // SET / session mode
  std::string name;
  std::string value;
};

// Parses a single statement.  Returns kSyntaxError for malformed input.
Status parse_statement(const std::string& sql, Statement* out);

// Splits a script into individual statements.  Semicolons inside string
// literals and comments do not terminate a statement.
std::vector<std::string> split_statements(const std::string& script);

}  // namespace mvdb
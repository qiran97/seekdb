// Execution of SELECT statements, including the vector top-k search.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

#include "engine.h"
#include "value.h"

namespace mvdb {

namespace {

// A resolved output expression.
struct ValueExtractor {
  enum class Kind { kColumn, kInteger, kText, kDistance };
  Kind kind = Kind::kInteger;
  int column = -1;
  int64_t number = 0;
  std::string text;
  std::vector<float> query_vector;
};

// A comparable sort key.
struct SortKey {
  enum class Type { kInteger, kReal, kText };
  Type type = Type::kText;
  int64_t number = 0;
  double real = 0.0;
  std::string text;
};

struct Projection {
  std::string name;
  ValueExtractor extractor;
};

struct OrderKey {
  ValueExtractor extractor;
  bool ascending = true;
};

struct Candidate {
  int64_t key = 0;
  std::vector<SortKey> keys;
  std::vector<ResultCell> cells;
};

Status resolve_expression(const Expr& expr, const TableDef& def,
                           ValueExtractor* out) {
  switch (expr.type) {
    case ExprType::kColumn: {
      int index = def.column_index(expr.name);
      if (index < 0) {
        return Status::Error(ErrorCode::kUnknownColumn,
                             "unknown column '" + expr.name + "' in '" +
                                 def.name + "'");
      }
      out->kind = ValueExtractor::Kind::kColumn;
      out->column = index;
      return Status::Ok();
    }
    case ExprType::kIntLiteral: {
      out->kind = ValueExtractor::Kind::kInteger;
      out->number = expr.int_value;
      return Status::Ok();
    }
    case ExprType::kStringLiteral: {
      out->kind = ValueExtractor::Kind::kText;
      out->text = expr.str_value;
      return Status::Ok();
    }
    case ExprType::kVectorLiteral:
    case ExprType::kL2Distance: {
      if (expr.children.size() != 2) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "L2_DISTANCE expects exactly two arguments");
      }
      const Expr& column_expr = *expr.children[0];
      const Expr& value_expr = *expr.children[1];
      if (column_expr.type != ExprType::kColumn) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "the first argument of L2_DISTANCE must be a vector column");
      }
      int index = def.column_index(column_expr.name);
      if (index < 0) {
        return Status::Error(ErrorCode::kUnknownColumn,
                             "unknown column '" + column_expr.name + "'");
      }
      if (index != def.vector_index) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "L2_DISTANCE requires the vector column of the table");
      }
      if (value_expr.type != ExprType::kStringLiteral) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "the second argument of L2_DISTANCE must be a vector literal");
      }
      VectorValue parsed;
      Status status = parse_vector_literal(value_expr.str_value,
                                           def.columns[index].dim, &parsed);
      if (!status.ok()) return status;
      out->kind = ValueExtractor::Kind::kDistance;
      out->column = index;
      out->query_vector = parsed.values;
      return Status::Ok();
    }
  }
  return Status::Error(ErrorCode::kSyntaxError, "unsupported expression");
}

std::string default_column_name(const Expr& expr) {
  switch (expr.type) {
    case ExprType::kColumn:
      return expr.name;
    case ExprType::kIntLiteral:
      return std::to_string(expr.int_value);
    case ExprType::kStringLiteral:
      return expr.str_value;
    case ExprType::kVectorLiteral:
    case ExprType::kL2Distance: {
      std::string name = "L2_DISTANCE(";
      if (!expr.children.empty()) {
        name += expr.children[0]->name;
        name += ", ";
      }
      if (expr.children.size() > 1) {
        name += "'" + expr.children[1]->str_value + "'";
      }
      name += ")";
      return name;
    }
  }
  return "expr";
}

ResultCell evaluate_extractor(const ValueExtractor& extractor, const TableDef& def,
                               const RowValues& row) {
  ResultCell cell;
  switch (extractor.kind) {
    case ValueExtractor::Kind::kInteger: {
      cell.type = ResultCell::Type::kInteger;
      cell.number = extractor.number;
      return cell;
    }
    case ValueExtractor::Kind::kText: {
      cell.type = ResultCell::Type::kText;
      cell.text = extractor.text;
      return cell;
    }
    case ValueExtractor::Kind::kDistance: {
      cell.type = ResultCell::Type::kReal;
      const std::vector<float>& stored = row.vector;
      size_t dim = std::min(stored.size(), extractor.query_vector.size());
      cell.real = l2_distance(stored.data(), extractor.query_vector.data(), dim);
      return cell;
    }
    case ValueExtractor::Kind::kColumn: {
      int index = extractor.column;
      if (index == def.pk_index) {
        cell.type = ResultCell::Type::kInteger;
        cell.number = row.key;
        return cell;
      }
      if (index == def.vector_index) {
        cell.type = ResultCell::Type::kVector;
        cell.vector = row.vector;
        return cell;
      }
      int position = value_position(def, index);
      cell.type = ResultCell::Type::kText;
      if (position >= 0 && position < static_cast<int>(row.strings.size())) {
        cell.text = row.strings[position];
      }
      if (def.columns[index].type == ColumnType::kBigInt) {
        cell.type = ResultCell::Type::kInteger;
        int64_t value = 0;
        if (parse_int64(trim(cell.text), &value)) cell.number = value;
      }
      return cell;
    }
  }
  return cell;
}

SortKey make_sort_key(const ValueExtractor& extractor, const TableDef& def,
                      const RowValues& row) {
  SortKey key;
  if (extractor.kind == ValueExtractor::Kind::kDistance) {
    key.type = SortKey::Type::kReal;
    const std::vector<float>& stored = row.vector;
    size_t dim = std::min(stored.size(), extractor.query_vector.size());
    key.real = l2_distance(stored.data(), extractor.query_vector.data(), dim);
    return key;
  }
  ResultCell cell = evaluate_extractor(extractor, def, row);
  switch (cell.type) {
    case ResultCell::Type::kInteger:
      key.type = SortKey::Type::kInteger;
      key.number = cell.number;
      break;
    case ResultCell::Type::kReal:
      key.type = SortKey::Type::kReal;
      key.real = cell.real;
      break;
    default:
      key.type = SortKey::Type::kText;
      key.text = cell.text;
      break;
  }
  return key;
}

int compare_sort_key(const SortKey& a, const SortKey& b) {
  if (a.type == SortKey::Type::kText || b.type == SortKey::Type::kText) {
    if (a.type != b.type) {
      // Mixed types are compared as text.
      std::string left = a.type == SortKey::Type::kText ? a.text : std::to_string(a.number);
      std::string right = b.type == SortKey::Type::kText ? b.text : std::to_string(b.number);
      int result = left.compare(right);
      return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }
    int result = a.text.compare(b.text);
    return result < 0 ? -1 : (result > 0 ? 1 : 0);
  }
  if (a.type == SortKey::Type::kInteger && b.type == SortKey::Type::kInteger) {
    if (a.number < b.number) return -1;
    if (a.number > b.number) return 1;
    return 0;
  }
  double left = a.type == SortKey::Type::kInteger ? static_cast<double>(a.number) : a.real;
  double right = b.type == SortKey::Type::kInteger ? static_cast<double>(b.number) : b.real;
  if (left < right) return -1;
  if (left > right) return 1;
  return 0;
}

// Compares two candidates.  A negative result means that `a` is the better
// candidate.  Ties are broken by the primary key in ascending order.
int compare_candidates(const SortKey* a, size_t a_count, int64_t a_key,
                       const SortKey* b, size_t b_count, int64_t b_key,
                       const std::vector<OrderKey>& order) {
  size_t count = a_count < b_count ? a_count : b_count;
  for (size_t i = 0; i < count; ++i) {
    int result = compare_sort_key(a[i], b[i]);
    if (result != 0) {
      bool ascending = order[i].ascending;
      // A negative result means that `a` is the better candidate: for an
      // ascending order the smaller key wins, for a descending order the
      // larger key wins.
      return ascending ? result : -result;
    }
  }
  if (a_key < b_key) return -1;
  if (a_key > b_key) return 1;
  return 0;
}

// A comparison of one column with a literal.  The common filter shapes are
// evaluated with this representation, it avoids the generic operand handling
// in the inner loop of a scan.
struct SimpleTerm {
  int position = -1;       // the position of the column in the value columns
  bool integer_column = false;
  int64_t number = 0;
  std::string text;
  CompareOp op = CompareOp::kEq;
};

// Converts a resolved condition into a conjunction of simple terms.  Returns
// false when the condition uses a shape that is not handled here.
bool build_simple_filter(const ResolvedCondition& condition, const TableDef& def,
                          std::vector<SimpleTerm>* terms) {
  if (condition.type == ConditionType::kAnd) {
    for (const ResolvedCondition& child : condition.children) {
      if (!build_simple_filter(child, def, terms)) return false;
    }
    return true;
  }
  if (condition.type != ConditionType::kCompare) return false;
  const ScalarOperand& left = condition.left;
  const ScalarOperand& right = condition.right;
  const ScalarOperand* column = nullptr;
  const ScalarOperand* literal = nullptr;
  if (left.kind == ScalarOperand::Kind::kColumn &&
      right.kind != ScalarOperand::Kind::kColumn) {
    column = &left;
    literal = &right;
  } else if (right.kind == ScalarOperand::Kind::kColumn &&
             left.kind != ScalarOperand::Kind::kColumn) {
    column = &right;
    literal = &left;
  } else {
    return false;
  }
  SimpleTerm term;
  term.op = condition.op;
  if (column->column == def.pk_index) {
    if (literal->kind != ScalarOperand::Kind::kInteger) return false;
    term.integer_column = true;
    term.number = literal->number;
  } else {
    if (literal->kind != ScalarOperand::Kind::kText) return false;
    if (column->column_type == ColumnType::kBigInt) return false;
    term.position = value_position(def, column->column);
    if (term.position < 0) return false;
    term.text = literal->text;
  }
  terms->push_back(std::move(term));
  return true;
}

// Evaluates the conjunction of simple terms against one row.
bool evaluate_simple_filter(const std::vector<SimpleTerm>& terms,
                             const TableDef& def, const RowValues& row) {
  (void)def;
  for (const SimpleTerm& term : terms) {
    bool matches = false;
    if (term.integer_column) {
      switch (term.op) {
        case CompareOp::kEq: matches = row.key == term.number; break;
        case CompareOp::kNe: matches = row.key != term.number; break;
        case CompareOp::kLt: matches = row.key < term.number; break;
        case CompareOp::kLe: matches = row.key <= term.number; break;
        case CompareOp::kGt: matches = row.key > term.number; break;
        case CompareOp::kGe: matches = row.key >= term.number; break;
      }
    } else {
      if (term.position >= static_cast<int>(row.strings.size())) return false;
      const std::string& value = row.strings[term.position];
      int comparison = value.compare(term.text);
      switch (term.op) {
        case CompareOp::kEq: matches = comparison == 0; break;
        case CompareOp::kNe: matches = comparison != 0; break;
        case CompareOp::kLt: matches = comparison < 0; break;
        case CompareOp::kLe: matches = comparison <= 0; break;
        case CompareOp::kGt: matches = comparison > 0; break;
        case CompareOp::kGe: matches = comparison >= 0; break;
      }
    }
    if (!matches) return false;
  }
  return true;
}

// A bounded top-k over a single numeric sort key.  The queue keeps the best
// candidates, its top is the worst one, so that a full queue can drop its
// top when a better candidate arrives.  The specialised loop avoids the
// generic candidate machinery for the most common query shape.
class NumericTopK {
 public:
  struct Entry {
    double key = 0.0;
    int64_t pk = 0;
    std::vector<ResultCell> cells;
  };

  NumericTopK(size_t keep, bool ascending)
      : keep_(keep), ascending_(ascending),
        heap_(Compare{ascending}) {}

  bool full() const { return heap_.size() >= keep_; }
  // True when the candidate is better than the worst entry of the queue.
  bool better(double key, int64_t pk) const {
    const Entry& worst = heap_.top();
    if (key != worst.key) return ascending_ ? key < worst.key : key > worst.key;
    return pk < worst.pk;
  }
  void pop() { heap_.pop(); }
  void push(Entry&& entry) { heap_.push(std::move(entry)); }
  // The entries in the order in which they have to be returned.
  std::vector<Entry> drain() {
    std::vector<Entry> result;
    result.reserve(heap_.size());
    while (!heap_.empty()) {
      result.push_back(std::move(const_cast<Entry&>(heap_.top())));
      heap_.pop();
    }
    std::reverse(result.begin(), result.end());
    return result;
  }

 private:
  struct Compare {
    bool ascending = true;
    // The priority queue keeps the "largest" element on top; a candidate that
    // is better has to compare smaller so that the worst entry ends up on top.
    bool operator()(const Entry& a, const Entry& b) const {
      if (a.key != b.key) return ascending ? (a.key < b.key) : (a.key > b.key);
      // The ties are broken by the primary key in ascending order.
      return a.pk < b.pk;
    }
  };

  size_t keep_;
  bool ascending_;
  std::priority_queue<Entry, std::vector<Entry>, Compare> heap_;
};

// The queue keeps the best candidates; its top is the worst candidate, so
// that a full queue can drop its top when a better candidate arrives.
struct CandidateOrder {
  const std::vector<OrderKey>* order = nullptr;
  bool operator()(const Candidate& a, const Candidate& b) const {
    // A candidate that is better compares smaller, therefore the largest
    // element of the queue (its top) is the worst candidate.
    return compare_candidates(a.keys.data(), a.keys.size(), a.key, b.keys.data(),
                              b.keys.size(), b.key, *order) < 0;
  }
};

}  // namespace

Status Engine::execute_select(const Statement& statement, ResultSet* result) {
  Table* table = find_table(statement.table);
  if (table == nullptr && !statement.table.empty()) {
    return Status::Error(ErrorCode::kUnknownTable,
                         "table '" + statement.table + "' does not exist");
  }
  if (table == nullptr) {
    // A projection without a table ("SELECT 1") evaluates a single row of
    // constant expressions.
    result->has_result_set = true;
    std::vector<ResultCell> row;
    for (const auto& expr : statement.select_list) {
      ResultCell cell;
      switch (expr->type) {
        case ExprType::kIntLiteral:
          cell.type = ResultCell::Type::kInteger;
          cell.number = expr->int_value;
          break;
        case ExprType::kStringLiteral:
          cell.type = ResultCell::Type::kText;
          cell.text = expr->str_value;
          break;
        default:
          return Status::Error(ErrorCode::kUnknownColumn,
                               "a column reference needs a table in the FROM "
                               "clause");
      }
      row.push_back(cell);
      result->columns.push_back(expr->alias.empty() ? default_column_name(*expr)
                                                    : expr->alias);
    }
    result->rows.push_back(std::move(row));
    return Status::Ok();
  }
  const TableDef& def = table->def;

  // --- projection ------------------------------------------------------
  std::vector<Projection> projections;
  if (statement.select_star) {
    for (size_t i = 0; i < def.columns.size(); ++i) {
      Projection projection;
      projection.name = def.columns[i].name;
      projection.extractor.kind = ValueExtractor::Kind::kColumn;
      projection.extractor.column = static_cast<int>(i);
      projections.push_back(std::move(projection));
    }
  } else {
    for (const auto& expr : statement.select_list) {
      Projection projection;
      Status status = resolve_expression(*expr, def, &projection.extractor);
      if (!status.ok()) return status;
      projection.name = expr->alias.empty() ? default_column_name(*expr) : expr->alias;
      projections.push_back(std::move(projection));
    }
  }

  // --- where -----------------------------------------------------------
  ResolvedCondition filter;
  bool has_filter = false;
  if (statement.where != nullptr) {
    Status status = resolve_condition(*statement.where, def, &filter);
    if (!status.ok()) return status;
    has_filter = true;
  }

  // The filter is evaluated with the specialised representation when it has
  // one of the common shapes.
  std::vector<SimpleTerm> simple_filter;
  if (has_filter) {
    build_simple_filter(filter, def, &simple_filter);
  }

  // --- order by --------------------------------------------------------
  std::vector<OrderKey> order;
  for (const OrderByItem& item : statement.order_by) {
    OrderKey key;
    if (item.ordinal > 0) {
      if (item.ordinal > static_cast<int>(projections.size())) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "ORDER BY position is out of range");
      }
      key.extractor = projections[item.ordinal - 1].extractor;
    } else if (item.expr != nullptr) {
      // An alias of the projection list wins over a column of the same name.
      const Projection* match = nullptr;
      if (item.expr->type == ExprType::kColumn && item.expr->alias.empty()) {
        for (const Projection& projection : projections) {
          if (!projection.name.empty() &&
              iequals(projection.name, item.expr->name) &&
              projection.extractor.kind == ValueExtractor::Kind::kColumn &&
              projection.extractor.column ==
                  def.column_index(item.expr->name)) {
            match = &projection;
            break;
          }
        }
        if (match == nullptr) {
          for (const Projection& projection : projections) {
            if (iequals(projection.name, item.expr->name)) {
              match = &projection;
              break;
            }
          }
        }
      }
      if (match != nullptr) {
        key.extractor = match->extractor;
      } else {
        Status status = resolve_expression(*item.expr, def, &key.extractor);
        if (!status.ok()) return status;
      }
    } else {
      return Status::Error(ErrorCode::kSyntaxError, "malformed ORDER BY clause");
    }
    key.ascending = item.ascending;
    order.push_back(std::move(key));
  }

  result->has_result_set = true;
  for (const Projection& projection : projections) {
    result->columns.push_back(projection.name);
  }

  const bool has_limit = statement.limit >= 0;
  const size_t keep = has_limit
                          ? static_cast<size_t>(statement.offset + statement.limit)
                          : 0;
  // A limit together with an order by uses a bounded heap, a limit without an
  // order by simply takes the first rows of the scan.
  const bool use_heap = has_limit && !order.empty();
  const bool use_limit_only = has_limit && order.empty();

  std::vector<Candidate> collected;
  std::priority_queue<Candidate, std::vector<Candidate>, CandidateOrder> heap(
      CandidateOrder{&order});
  SortKey key_buffer[8];

  // The common shape of a query is a single numeric sort key with a limit.
  // It is executed by a specialised loop that computes the key directly and
  // skips the generic candidate machinery for the rows that do not enter the
  // queue.
  bool numeric_key = false;
  bool integer_key = false;
  if (use_heap && order.size() == 1) {
    const ValueExtractor& extractor = order[0].extractor;
    if (extractor.kind == ValueExtractor::Kind::kDistance) {
      numeric_key = true;
    } else if (extractor.kind == ValueExtractor::Kind::kColumn &&
               extractor.column == def.pk_index) {
      numeric_key = true;
      integer_key = true;
    }
  }
  if (numeric_key) {
    NumericTopK top_k(keep, order[0].ascending);
    const ValueExtractor& extractor = order[0].extractor;
    Status status = scan_rows(table, [&](int64_t key, const RowValues& row) {
      if (has_filter) {
        if (!simple_filter.empty()) {
          if (!evaluate_simple_filter(simple_filter, def, row)) return true;
        } else if (!evaluate_condition(filter, def, row)) {
          return true;
        }
      }
      double sort_key = 0.0;
      if (integer_key) {
        sort_key = static_cast<double>(key);
      } else {
        const std::vector<float>& stored = row.vector;
        size_t dim = std::min(stored.size(), extractor.query_vector.size());
        sort_key = l2_distance(stored.data(), extractor.query_vector.data(), dim);
      }
      if (top_k.full()) {
        if (!top_k.better(sort_key, key)) return true;
        top_k.pop();
      }
      NumericTopK::Entry entry;
      entry.key = sort_key;
      entry.pk = key;
      for (const Projection& projection : projections) {
        entry.cells.push_back(evaluate_extractor(projection.extractor, def, row));
      }
      top_k.push(std::move(entry));
      return true;
    });
    if (!status.ok()) return status;
    std::vector<NumericTopK::Entry> winners = top_k.drain();
    size_t skipped = 0;
    for (NumericTopK::Entry& entry : winners) {
      if (skipped < static_cast<size_t>(statement.offset)) {
        ++skipped;
        continue;
      }
      if (result->rows.size() >= static_cast<size_t>(statement.limit)) break;
      result->rows.push_back(std::move(entry.cells));
    }
    return Status::Ok();
  }

  Status status = scan_rows(table, [&](int64_t key, const RowValues& row) {
    if (has_filter && !evaluate_condition(filter, def, row)) return true;
    if (use_limit_only && collected.size() >= keep) return false;
    size_t key_count = 0;
    for (const OrderKey& order_key : order) {
      if (key_count >= sizeof(key_buffer) / sizeof(key_buffer[0])) break;
      key_buffer[key_count++] = make_sort_key(order_key.extractor, def, row);
    }
    if (use_heap && heap.size() >= keep) {
      const Candidate& worst = heap.top();
      int comparison = compare_candidates(worst.keys.data(), worst.keys.size(),
                                          worst.key, key_buffer, key_count, key,
                                          order);
      if (comparison <= 0) {
        // The candidate is not better than the worst entry of the heap.
        return true;
      }
      heap.pop();
    }
    Candidate candidate;
    candidate.key = key;
    candidate.keys.assign(key_buffer, key_buffer + key_count);
    for (const Projection& projection : projections) {
      candidate.cells.push_back(evaluate_extractor(projection.extractor, def, row));
    }
    if (use_heap) {
      heap.push(std::move(candidate));
    } else {
      collected.push_back(std::move(candidate));
    }
    return true;
  });
  if (!status.ok()) return status;

  if (use_heap) {
    while (!heap.empty()) {
      collected.push_back(std::move(heap.top()));
      heap.pop();
    }
    // The heap yields the worst candidates first.
    std::reverse(collected.begin(), collected.end());
  } else if (!order.empty()) {
    std::stable_sort(collected.begin(), collected.end(),
                     [&order](const Candidate& a, const Candidate& b) {
                       for (size_t i = 0; i < order.size(); ++i) {
                         int result = compare_sort_key(a.keys[i], b.keys[i]);
                         if (result != 0) {
                           return order[i].ascending ? result < 0 : result > 0;
                         }
                       }
                       return a.key < b.key;
                     });
  }

  size_t skipped = 0;
  for (Candidate& candidate : collected) {
    if (has_limit) {
      if (skipped < static_cast<size_t>(statement.offset)) {
        ++skipped;
        continue;
      }
      if (result->rows.size() >= static_cast<size_t>(statement.limit)) break;
    }
    result->rows.push_back(std::move(candidate.cells));
  }
  return Status::Ok();
}

}  // namespace mvdb
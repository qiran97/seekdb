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
      return ascending ? -result : result;
    }
  }
  if (a_key < b_key) return -1;
  if (a_key > b_key) return 1;
  return 0;
}

// The queue keeps the best candidates; the top of the queue is the worst one.
struct CandidateOrder {
  const std::vector<OrderKey>* order = nullptr;
  bool operator()(const Candidate& a, const Candidate& b) const {
    return compare_candidates(a.keys.data(), a.keys.size(), a.key, b.keys.data(),
                              b.keys.size(), b.key, *order) > 0;
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
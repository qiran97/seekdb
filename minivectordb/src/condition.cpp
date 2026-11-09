// Resolution and evaluation of the WHERE clause.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "engine.h"

namespace mvdb {

int value_position(const TableDef& def, int column_index) {
  int position = 0;
  for (size_t i = 0; i < def.columns.size(); ++i) {
    int index = static_cast<int>(i);
    if (index == def.pk_index || index == def.vector_index) continue;
    if (index == column_index) return position;
    ++position;
  }
  return position;
}

namespace {

Status resolve_operand(const Expr& expr, const TableDef& def,
                       ScalarOperand* out) {
  switch (expr.type) {
    case ExprType::kColumn: {
      int index = def.column_index(expr.name);
      if (index < 0) {
        return Status::Error(ErrorCode::kUnknownColumn,
                             "unknown column '" + expr.name + "'");
      }
      if (def.columns[index].type == ColumnType::kVector) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "vector column '" + expr.name +
                                 "' cannot be used in a comparison");
      }
      out->kind = ScalarOperand::Kind::kColumn;
      out->column = index;
      out->column_type = def.columns[index].type;
      return Status::Ok();
    }
    case ExprType::kIntLiteral: {
      out->kind = ScalarOperand::Kind::kInteger;
      out->number = expr.int_value;
      return Status::Ok();
    }
    case ExprType::kStringLiteral: {
      out->kind = ScalarOperand::Kind::kText;
      out->text = expr.str_value;
      return Status::Ok();
    }
    default:
      return Status::Error(ErrorCode::kSyntaxError,
                           "unsupported operand in a comparison");
  }
}

struct OperandValue {
  bool numeric = false;
  int64_t number = 0;
  // The text is a view on the row, comparing it must not copy the value.
  const std::string* text = nullptr;
  std::string empty;
};

int64_t leading_number(const std::string& text) {
  size_t index = 0;
  while (index < text.size() && isspace(static_cast<unsigned char>(text[index]))) ++index;
  bool negative = false;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    negative = text[index] == '-';
    ++index;
  }
  int64_t value = 0;
  bool any = false;
  while (index < text.size() && isdigit(static_cast<unsigned char>(text[index]))) {
    value = value * 10 + (text[index] - '0');
    ++index;
    any = true;
  }
  if (index < text.size() && text[index] == '.') {
    ++index;
    while (index < text.size() && isdigit(static_cast<unsigned char>(text[index]))) {
      ++index;
      any = true;
    }
  }
  if (!any) return 0;
  return negative ? -value : value;
}

OperandValue operand_value(const ScalarOperand& operand, const TableDef& def,
                           const RowValues& row) {
  OperandValue value;
  switch (operand.kind) {
    case ScalarOperand::Kind::kInteger:
      value.numeric = true;
      value.number = operand.number;
      return value;
    case ScalarOperand::Kind::kText:
      value.numeric = false;
      value.text = &operand.text;
      return value;
    case ScalarOperand::Kind::kColumn: {
      if (operand.column == def.pk_index) {
        value.numeric = true;
        value.number = row.key;
        return value;
      }
      int position = value_position(def, operand.column);
      if (position < 0 || position >= static_cast<int>(row.strings.size())) {
        value.numeric = false;
        value.empty.clear();
        value.text = &value.empty;
        return value;
      }
      const std::string& text = row.strings[position];
      if (operand.column_type == ColumnType::kBigInt) {
        value.numeric = true;
        value.number = leading_number(text);
        return value;
      }
      value.numeric = false;
      value.text = &text;
      return value;
    }
  }
  value.numeric = false;
  return value;
}

int compare_operands(const ScalarOperand& left_operand,
                      const ScalarOperand& right_operand, const TableDef& def,
                      const RowValues& row) {
  OperandValue left = operand_value(left_operand, def, row);
  OperandValue right = operand_value(right_operand, def, row);
  if (left.numeric && !right.numeric) {
    right.numeric = true;
    right.number = leading_number(*right.text);
  } else if (!left.numeric && right.numeric) {
    left.numeric = true;
    left.number = leading_number(*left.text);
  }
  if (left.numeric && right.numeric) {
    if (left.number < right.number) return -1;
    if (left.number > right.number) return 1;
    return 0;
  }
  int result = left.text->compare(*right.text);
  if (result < 0) return -1;
  if (result > 0) return 1;
  return 0;
}

}  // namespace

Status resolve_condition(const Condition& condition, const TableDef& def,
                          ResolvedCondition* out) {
  switch (condition.type) {
    case ConditionType::kAnd:
    case ConditionType::kOr: {
      out->type = condition.type;
      for (const auto& child : condition.children) {
        ResolvedCondition resolved;
        Status status = resolve_condition(*child, def, &resolved);
        if (!status.ok()) return status;
        out->children.push_back(std::move(resolved));
      }
      return Status::Ok();
    }
    case ConditionType::kNot: {
      out->type = ConditionType::kNot;
      if (condition.children.empty()) {
        return Status::Error(ErrorCode::kSyntaxError, "malformed NOT expression");
      }
      ResolvedCondition resolved;
      Status status = resolve_condition(*condition.children[0], def, &resolved);
      if (!status.ok()) return status;
      out->children.push_back(std::move(resolved));
      return Status::Ok();
    }
    case ConditionType::kCompare: {
      out->type = ConditionType::kCompare;
      out->op = condition.op;
      Status status = resolve_operand(*condition.left, def, &out->left);
      if (!status.ok()) return status;
      status = resolve_operand(*condition.right, def, &out->right);
      if (!status.ok()) return status;
      return Status::Ok();
    }
  }
  return Status::Error(ErrorCode::kSyntaxError, "malformed condition");
}

bool evaluate_condition(const ResolvedCondition& condition, const TableDef& def,
                        const RowValues& row) {
  switch (condition.type) {
    case ConditionType::kAnd: {
      for (const ResolvedCondition& child : condition.children) {
        if (!evaluate_condition(child, def, row)) return false;
      }
      return true;
    }
    case ConditionType::kOr: {
      for (const ResolvedCondition& child : condition.children) {
        if (evaluate_condition(child, def, row)) return true;
      }
      return false;
    }
    case ConditionType::kNot: {
      if (condition.children.empty()) return false;
      return !evaluate_condition(condition.children[0], def, row);
    }
    case ConditionType::kCompare: {
      int result = compare_operands(condition.left, condition.right, def, row);
      switch (condition.op) {
        case CompareOp::kEq: return result == 0;
        case CompareOp::kNe: return result != 0;
        case CompareOp::kLt: return result < 0;
        case CompareOp::kLe: return result <= 0;
        case CompareOp::kGt: return result > 0;
        case CompareOp::kGe: return result >= 0;
      }
      return false;
    }
  }
  return false;
}

}  // namespace mvdb
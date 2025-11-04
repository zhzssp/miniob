/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/07/05.
//

#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/expr/arithmetic_operator.hpp"
#include "sql/parser/parse_defs.h"
#include "sql/operator/physical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/stmt/select_stmt.h"
#include "sql/expr/subquery_expr.h"

using namespace std;

RC FieldExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(table_name(), field_name()), value);
}

bool FieldExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::FIELD) {
    return false;
  }
  const auto &other_field_expr = static_cast<const FieldExpr &>(other);
  return table_name() == other_field_expr.table_name() && field_name() == other_field_expr.field_name();
}

// TODO: 在进行表达式计算时，`chunk` 包含了所有列，因此可以通过 `field_id` 获取到对应列。
// 后续可以优化成在 `FieldExpr` 中存储 `chunk` 中某列的位置信息。
RC FieldExpr::get_column(Chunk &chunk, Column &column)
{
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    column.reference(chunk.column(field().meta()->field_id()));
  }
  return RC::SUCCESS;
}

bool ValueExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::VALUE) {
    return false;
  }
  const auto &other_value_expr = static_cast<const ValueExpr &>(other);
  return value_.compare(other_value_expr.get_value()) == 0;
}

RC ValueExpr::get_value(const Tuple &tuple, Value &value) const
{
  value = value_;
  return RC::SUCCESS;
}

RC ValueExpr::get_column(Chunk &chunk, Column &column)
{
  column.init(value_, chunk.rows());
  return RC::SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
CastExpr::CastExpr(unique_ptr<Expression> child, AttrType cast_type) : child_(std::move(child)), cast_type_(cast_type)
{}

CastExpr::~CastExpr() {}

RC CastExpr::cast(const Value &value, Value &cast_value) const
{
  RC rc = RC::SUCCESS;
  if (this->value_type() == value.attr_type()) {
    cast_value = value;
    return rc;
  }
  rc = Value::cast_to(value, cast_type_, cast_value);
  return rc;
}

RC CastExpr::get_value(const Tuple &tuple, Value &result) const
{
  Value value;
  RC rc = child_->get_value(tuple, value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

RC CastExpr::get_column(Chunk &chunk, Column &column)
{
  Column child_column;
  RC rc = child_->get_column(chunk, child_column);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  column.init(cast_type_, child_column.attr_len());
  for (int i = 0; i < child_column.count(); ++i) {
    Value value = child_column.get_value(i);
    Value cast_value;
    rc = cast(value, cast_value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    column.append_value(cast_value);
  }
  return rc;
}

RC CastExpr::try_get_value(Value &result) const
{
  Value value;
  RC rc = child_->try_get_value(value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

////////////////////////////////////////////////////////////////////////////////

ComparisonExpr::ComparisonExpr(CompOp comp, Expression *left, Expression *right)
    : comp_(comp), left_(left), right_(right)
{}

ComparisonExpr::ComparisonExpr(CompOp comp, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : comp_(comp), left_(std::move(left)), right_(std::move(right))
{
}

ComparisonExpr::~ComparisonExpr() {}

RC ComparisonExpr::compare_value(const Value &left, const Value &right, bool &result) const
{
  RC  rc         = RC::SUCCESS;
  int cmp_result = 0;
  
  // is / is not --> 第二个数只能是null
  if (comp_ == IS_OP) {
    if (!left.is_null() && right.is_null()) {
      result = false;
      return rc;
    } else if (left.is_null() && right.is_null()) {
      result = true;
      return rc;
    }
    else {
      return RC::INVALID_DATE_FORMAT;
    }
  } else if(comp_ == IS_NOT_OP) {
    if (!left.is_null() && right.is_null()) {
      result = true;
      return rc;
    } else if (left.is_null() && right.is_null()) {
      result = false;
      return rc;
    } else {
      return RC::INVALID_DATE_FORMAT;
    }
  } else {
    // 其余全部当作传统运算符对待
    if(left.is_null() || right.is_null()) {
      result = false;
      return rc;
    }
  }
  LOG_INFO("ComparisonExpr's comp_ is not related to is null / is not null");

  // 安全类型对齐：在比较前尽量将不同类型转换为可比较的同一类型

  // 规则：
  // - INTS vs FLOATS -> 都转为 FLOATS
  // - CHARS vs numeric -> 若可解析为数字，则都转为 FLOATS；否则按不可比处理（结果为 false）
  // - 其他不同类型 -> 不可比（结果为 false）
  AttrType lt = left.attr_type();
  AttrType rt = right.attr_type();

  if (lt != rt) {
    // INTS/FLOATS 对齐为浮点比较
    if ((lt == AttrType::INTS && rt == AttrType::FLOATS) || (lt == AttrType::FLOATS && rt == AttrType::INTS)) {
      Value l2 = left;
      Value r2 = right;
      if (lt == AttrType::INTS) {
        // 提升左为float
        Value tmp;
        tmp.set_float(static_cast<float>(left.get_int()));
        l2 = tmp;
      }
      if (rt == AttrType::INTS) {
        Value tmp;
        tmp.set_float(static_cast<float>(right.get_int()));
        r2 = tmp;
      }
      cmp_result = l2.compare(r2);
    } else if ((lt == AttrType::CHARS && (rt == AttrType::INTS || rt == AttrType::FLOATS)) ||
               (rt == AttrType::CHARS && (lt == AttrType::INTS || lt == AttrType::FLOATS))) {
      // 解析字符串为数字
      auto parse_to_double = [](const Value &v, double &out, bool &ok) {
        ok = false;
        if (v.attr_type() == AttrType::FLOATS) {
          out = static_cast<double>(v.get_float());
          ok = true;
          return;
        }
        if (v.attr_type() == AttrType::INTS) {
          out = static_cast<double>(v.get_int());
          ok = true;
          return;
        }
        if (v.attr_type() == AttrType::CHARS) {
          std::string s = v.to_string();
          // 去除可能的引号
          if (!s.empty() && s.front() == '\'' && s.back() == '\'' && s.size() >= 2) {
            s = s.substr(1, s.size() - 2);
          }
          char *endptr = nullptr;
          const char *cstr = s.c_str();
          errno = 0;
          double val = strtod(cstr, &endptr);
          if (errno == 0 && endptr != cstr) {
            // 允许部分数字解析，如 '16a' -> 16.0
            out = val;
            ok = true;
          } else if (s.size() == 1) {
            // 单字符，按 ASCII 码进行数值比较
            out = static_cast<unsigned char>(s[0]);
            ok = true;
          }
          return;
        }
      };

      double dl = 0.0, dr = 0.0;
      bool ok_l = false, ok_r = false;
      parse_to_double(left, dl, ok_l);
      parse_to_double(right, dr, ok_r);
      if (ok_l && ok_r) {
        Value l2; l2.set_float(static_cast<float>(dl));
        Value r2; r2.set_float(static_cast<float>(dr));
        cmp_result = l2.compare(r2);
      } else {
        // 无法比较的异类型，返回 false 结果
        cmp_result = 0;
        switch (comp_) {
          case EQUAL_TO:      result = false; return RC::SUCCESS;
          case NOT_EQUAL:     result = false; return RC::SUCCESS;
          case LESS_EQUAL:    result = false; return RC::SUCCESS;
          case LESS_THAN:     result = false; return RC::SUCCESS;
          case GREAT_EQUAL:   result = false; return RC::SUCCESS;
          case GREAT_THAN:    result = false; return RC::SUCCESS;
          default: break;
        }
      }
    } else {
      // 未支持的跨类型比较，按不可比处理
      cmp_result = 0;
      switch (comp_) {
        case EQUAL_TO:      result = false; return RC::SUCCESS;
        case NOT_EQUAL:     result = false; return RC::SUCCESS;
        case LESS_EQUAL:    result = false; return RC::SUCCESS;
        case LESS_THAN:     result = false; return RC::SUCCESS;
        case GREAT_EQUAL:   result = false; return RC::SUCCESS;
        case GREAT_THAN:    result = false; return RC::SUCCESS;
        default: break;
      }
    }
  } else {
    // 相同类型，直接比较
    cmp_result = left.compare(right);
  }
  result         = false;
  switch (comp_) {
    case EQUAL_TO: {
      result = (cmp_result == 0);
    } break;
    case LESS_EQUAL: {
      result = (cmp_result <= 0);
    } break;
    case NOT_EQUAL: {
      result = (cmp_result != 0);
    } break;
    case LESS_THAN: {
      result = (cmp_result < 0);
    } break;
    case GREAT_EQUAL: {
      result = (cmp_result >= 0);
    } break;
    case GREAT_THAN: {
      result = (cmp_result > 0);
    } break;
    default: {
      LOG_WARN("unsupported comparison. %d", comp_);
      rc = RC::INTERNAL;
    } break;
  }

  return rc;
}

RC ComparisonExpr::try_get_value(Value &cell) const
{
  if (left_->type() == ExprType::VALUE && right_->type() == ExprType::VALUE) {
    ValueExpr *  left_value_expr  = static_cast<ValueExpr *>(left_.get());
    ValueExpr *  right_value_expr = static_cast<ValueExpr *>(right_.get());
    const Value &left_cell        = left_value_expr->get_value();
    const Value &right_cell       = right_value_expr->get_value();

    bool value = false;
    RC   rc    = compare_value(left_cell, right_cell, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
    } else {
      cell.set_boolean(value);
    }
    return rc;
  }

  return RC::INVALID_ARGUMENT;
}

RC ComparisonExpr::get_value(const Tuple &tuple, Value &value) const
{
  // Handle IN and NOT IN operations specially
  if (comp_ == IN_OP || comp_ == NOT_IN_OP) {
    LOG_WARN("IN operation: comp_=%d, right type=%d, left type=%d, right_ptr=%p, left_ptr=%p", 
             (int)comp_, (int)right_->type(), (int)left_->type(), right_.get(), left_.get());
    
    Value left_value;
    RC rc = left_->get_value(tuple, left_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }
    LOG_WARN("[IN CHECK] lhs=%s", left_value.to_string().c_str());
    bool found = false;
    bool any_equal = false;
    int sub_count = 0;
    if (right_->type() != ExprType::SUB_QUERY) {
      LOG_WARN("IN operation expected SUB_QUERY on right, got type=%d. Treating as not found.", (int)right_->type());
      value.set_boolean(false);
      return RC::SUCCESS;
    }
    SubqueryExpr* subquery_expr = static_cast<SubqueryExpr*>(right_.get());
    Tuple *outer_tuple_ptr = const_cast<Tuple*>(&tuple);
    rc = subquery_expr->open_physical_operator(outer_tuple_ptr);
    if (rc != RC::SUCCESS) {
      LOG_WARN("IN: failed to open subquery operator. rc=%s", strrc(rc));
      value.set_boolean(false);
      return RC::SUCCESS;
    }
    while ((rc = subquery_expr->physical_operator()->next()) == RC::SUCCESS) {
      ++sub_count;
      auto sub_t = subquery_expr->physical_operator()->current_tuple();
      LOG_WARN("[IN CHECK] subquery row %d, cell_num=%d", sub_count, sub_t ? sub_t->cell_num() : -1);
      if (!sub_t || sub_t->cell_num() == 0) continue;
      Value right_cell;
      sub_t->cell_at(0, right_cell);
      LOG_WARN("[IN CHECK]   lhs=%s subval=%s", left_value.to_string().c_str(), right_cell.to_string().c_str());
      Value aligned_left = left_value;
      Value aligned_right = right_cell;
      Value casted;
      if (right_cell.attr_type() != left_value.attr_type()) {
        if (Value::cast_to(right_cell, left_value.attr_type(), casted) == RC::SUCCESS) {
          aligned_right = casted;
        } else if (Value::cast_to(left_value, right_cell.attr_type(), casted) == RC::SUCCESS) {
          aligned_left = casted;
        }
      }
      if ((aligned_left.attr_type() == AttrType::INTS || aligned_left.attr_type() == AttrType::FLOATS || aligned_left.attr_type() == AttrType::CHARS) &&
          (aligned_right.attr_type() == AttrType::INTS || aligned_right.attr_type() == AttrType::FLOATS || aligned_right.attr_type() == AttrType::CHARS)) {
        if (aligned_left.compare(aligned_right) == 0) {
          any_equal = true;
          if (comp_ == IN_OP) {
            found = true;
            break;
          }
        }
      }
    }
    if (sub_count == 0)
      LOG_WARN("[IN CHECK] subquery yielded no rows!");
    RC close_rc = subquery_expr->close_physical_operator();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("IN: failed to close subquery operator. rc=%s", strrc(close_rc));
    }
    if (rc != RC::SUCCESS && rc != RC::RECORD_EOF) {
      LOG_WARN("IN: subquery execution error. rc=%s", strrc(rc));
    }
    found = (comp_ == IN_OP) ? any_equal : !any_equal;
    LOG_WARN("[IN CHECK] lhs=%s final found=%d (any_equal=%d) on sub_count=%d", left_value.to_string().c_str(), found, any_equal, sub_count);
    value.set_boolean(found);
    return RC::SUCCESS;
  }
  
  Value left_value;
  Value right_value;

  // 比较表达式树 --> 递归
  RC rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc))
    ;
    return rc;
  }

  bool bool_value = false;

  rc = compare_value(left_value, right_value, bool_value);
  if (rc == RC::SUCCESS) {
    value.set_boolean(bool_value);
  }
  return rc;
}

RC ComparisonExpr::eval(Chunk &chunk, vector<uint8_t> &select)
{
  RC     rc = RC::SUCCESS;
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  if (left_column.attr_type() != right_column.attr_type()) {
    LOG_WARN("cannot compare columns with different types");
    return RC::INTERNAL;
  }
  if (left_column.attr_type() == AttrType::INTS) {
    rc = compare_column<int>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::FLOATS) {
    rc = compare_column<float>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::CHARS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool        result   = false;
      rc                   = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }

  } else {
    LOG_WARN("unsupported data type %d", left_column.attr_type());
    return RC::INTERNAL;
  }
  return rc;
}

template <typename T>
RC ComparisonExpr::compare_column(const Column &left, const Column &right, vector<uint8_t> &result) const
{
  RC rc = RC::SUCCESS;

  bool left_const  = left.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    compare_result<T, true, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else if (left_const && !right_const) {
    compare_result<T, true, false>((T *)left.data(), (T *)right.data(), right.count(), result, comp_);
  } else if (!left_const && right_const) {
    compare_result<T, false, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else {
    compare_result<T, false, false>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
ConjunctionExpr::ConjunctionExpr(Type type, vector<unique_ptr<Expression>> &children)
    : conjunction_type_(type), children_(std::move(children))
{}

RC ConjunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;
  if (children_.empty()) {
    value.set_boolean(true);
    return rc;
  }

  Value tmp_value;
  for (const unique_ptr<Expression> &expr : children_) {
    rc = expr->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value by child expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = tmp_value.get_boolean();
    if ((conjunction_type_ == Type::AND && !bool_value) || (conjunction_type_ == Type::OR && bool_value)) {
      value.set_boolean(bool_value);
      return rc;
    }
  }

  bool default_value = (conjunction_type_ == Type::AND);
  value.set_boolean(default_value);
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, Expression *left, Expression *right)
    : arithmetic_type_(type), left_(left), right_(right)
{}
ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : arithmetic_type_(type), left_(std::move(left)), right_(std::move(right))
{}

bool ArithmeticExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (type() != other.type()) {
    return false;
  }
  auto &other_arith_expr = static_cast<const ArithmeticExpr &>(other);
  return arithmetic_type_ == other_arith_expr.arithmetic_type() && left_->equal(*other_arith_expr.left_) &&
         right_->equal(*other_arith_expr.right_);
}
AttrType ArithmeticExpr::value_type() const
{
  if (!left_) {
    return right_->value_type();
  }
  if (!right_) {
    return left_->value_type();
  }

  if ((left_->value_type() == AttrType::INTS) &&
   (right_->value_type() == AttrType::INTS) &&
      arithmetic_type_ != Type::DIV) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value, Value &value) const
{
  RC rc = RC::SUCCESS;
  if(left_value.is_null() || right_value.is_null()) 
  {
    value.set_null(true);
    return rc;
  }
  
  const AttrType target_type = value_type();
  value.set_type(target_type);

  switch (arithmetic_type_) {
    case Type::ADD: {
      Value::add(left_value, right_value, value);
    } break;

    case Type::SUB: {
      Value::subtract(left_value, right_value, value);
    } break;

    case Type::MUL: {
      Value::multiply(left_value, right_value, value);
    } break;

    case Type::DIV: {
      Value::divide(left_value, right_value, value);
    } break;

    case Type::NEGATIVE: {
      Value::negative(right_value, value);
    } break;

    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported arithmetic type. %d", arithmetic_type_);
    } break;
  }
  return rc;
}

template <bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
RC ArithmeticExpr::execute_calc(
    const Column &left, const Column &right, Column &result, Type type, AttrType attr_type) const
{
  RC rc = RC::SUCCESS;
  switch (type) {
    case Type::ADD: {
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, AddOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, AddOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::SUB:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, SubtractOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, SubtractOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::MUL:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, MultiplyOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, MultiplyOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::DIV:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, DivideOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, DivideOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::NEGATIVE:
      if (attr_type == AttrType::INTS) {
        unary_operator<LEFT_CONSTANT, int, NegateOperator>((int *)left.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        unary_operator<LEFT_CONSTANT, float, NegateOperator>(
            (float *)left.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    default: rc = RC::UNIMPLEMENTED; break;
  }
  if (rc == RC::SUCCESS) {
    result.set_count(result.capacity());
  }
  return rc;
}

RC ArithmeticExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  if (left_) {
    rc = left_->get_value(tuple, left_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }
  }
  if (right_) {
    rc = right_->get_value(tuple, right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }
  return calc_value(left_value, right_value, value);
}

RC ArithmeticExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
    return rc;
  }
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of right expression. rc=%s", strrc(rc));
    return rc;
  }
  return calc_column(left_column, right_column, column);
}

RC ArithmeticExpr::calc_column(const Column &left_column, const Column &right_column, Column &column) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();
  column.init(target_type, left_column.attr_len(), max(left_column.count(), right_column.count()));
  bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    column.set_column_type(Column::Type::CONSTANT_COLUMN);
    rc = execute_calc<true, true>(left_column, right_column, column, arithmetic_type_, target_type);
  } else if (left_const && !right_const) {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
  } else if (!left_const && right_const) {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, true>(left_column, right_column, column, arithmetic_type_, target_type);
  } else {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
  }
  return rc;
}

RC ArithmeticExpr::try_get_value(Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  if (left_) {
    rc = left_->try_get_value(left_value);
    if (rc != RC::SUCCESS) {
      LOG_ERROR("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  if (right_) {
    rc = right_->try_get_value(right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

////////////////////////////////////////////////////////////////////////////////

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, Expression *child)
    : aggregate_name_(aggregate_name), child_(child)
{}

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, unique_ptr<Expression> child)
    : aggregate_name_(aggregate_name), child_(std::move(child))
{}

////////////////////////////////////////////////////////////////////////////////
AggregateExpr::AggregateExpr(Type type, Expression *child) : aggregate_type_(type), child_(child) {}

AggregateExpr::AggregateExpr(Type type, unique_ptr<Expression> child) : aggregate_type_(type), child_(std::move(child))
{}

RC AggregateExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    rc = RC::INTERNAL;
  }
  return rc;
}

bool AggregateExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != type()) {
    return false;
  }
  const AggregateExpr &other_aggr_expr = static_cast<const AggregateExpr &>(other);
  return aggregate_type_ == other_aggr_expr.aggregate_type() && child_->equal(*other_aggr_expr.child());
}

unique_ptr<Aggregator> AggregateExpr::create_aggregator() const
{
  unique_ptr<Aggregator> aggregator;
  switch (aggregate_type_) {
    case Type::SUM: {
      aggregator = make_unique<SumAggregator>();
      break;
    }
    case Type::COUNT: {
      aggregator = make_unique<CountAggregator>();
      break;
    }
    case Type::AVG: {
      aggregator = make_unique<AvgAggregator>();
      break;
    }
    case Type::MAX: {
      aggregator = make_unique<MaxAggregator>();
      break;
    }
    case Type::MIN: {
      aggregator = make_unique<MinAggregator>();
      break;
    }
    default: {
      ASSERT(false, "unsupported aggregate type");
      break;
    }
  }
  return aggregator;
}

RC AggregateExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(name()), value);
}

RC AggregateExpr::type_from_string(const char *type_str, AggregateExpr::Type &type)
{
  RC rc = RC::SUCCESS;
  if (0 == strcasecmp(type_str, "count")) {
    type = Type::COUNT;
  } else if (0 == strcasecmp(type_str, "sum")) {
    type = Type::SUM;
  } else if (0 == strcasecmp(type_str, "avg")) {
    type = Type::AVG;
  } else if (0 == strcasecmp(type_str, "max")) {
    type = Type::MAX;
  } else if (0 == strcasecmp(type_str, "min")) {
    type = Type::MIN;
  } else {
    rc = RC::INVALID_ARGUMENT;
  }
  return rc;
}

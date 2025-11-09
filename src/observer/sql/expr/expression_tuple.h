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
// Created by Wangyunlai on 2024/5/31.
//

#pragma once

#include "common/lang/vector.h"
#include "sql/expr/tuple.h"
#include "common/value.h"
#include "common/sys/rc.h"
#include "common/log/log.h"

template <typename ExprPointerType>
class ExpressionTuple : public Tuple
{
public:
  ExpressionTuple(const vector<ExprPointerType> &expressions) : expressions_(expressions) {}
  virtual ~ExpressionTuple() = default;

  void set_tuple(const Tuple *tuple) { child_tuple_ = tuple; }

  int cell_num() const override { return static_cast<int>(expressions_.size()); }

  RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= cell_num()) {
      return RC::INVALID_ARGUMENT;
    }

    if (child_tuple_ == nullptr) {
      LOG_WARN("ExpressionTuple::cell_at: child_tuple_ is nullptr, cannot evaluate expression");
      return RC::INTERNAL;
    }

    const ExprPointerType &expression = expressions_[index];
    return get_value(expression, cell);
  }

  RC spec_at(int index, TupleCellSpec &spec) const override
  {
    if (index < 0 || index >= cell_num()) {
      return RC::INVALID_ARGUMENT;
    }

    const ExprPointerType &expression = expressions_[index];
    spec                              = TupleCellSpec(expression->name());
    return RC::SUCCESS;
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    RC rc = RC::SUCCESS;
    if (child_tuple_ != nullptr) {
      rc = child_tuple_->find_cell(spec, cell);
      if (OB_SUCC(rc)) {
        return rc;
      }
    }

    rc = RC::NOTFOUND;
    for (const ExprPointerType &expression : expressions_) {
      if (0 == strcmp(spec.alias(), expression->name())) {
        rc = get_value(expression, cell);
        break;
      }
    }

    return rc;
  }

private:
  RC get_value(const ExprPointerType &expression, Value &value) const
  {
    RC rc = RC::SUCCESS;
    if (child_tuple_ != nullptr) {
      rc = expression->get_value(*child_tuple_, value);
      if (rc != RC::SUCCESS) {
        LOG_WARN("ExpressionTuple::get_value: expression->get_value failed. rc=%s, expr_type=%d, child_tuple_=%p", 
                 strrc(rc), (int)expression->type(), child_tuple_);
      }
    } else {
      // 如果 child_tuple_ 为 nullptr，尝试使用 try_get_value
      // 但对于 FieldExpr 等需要 tuple 的表达式，这可能会失败
      rc = expression->try_get_value(value);
      if (rc != RC::SUCCESS) {
        // 如果 try_get_value 失败，返回更明确的错误
        // 这通常意味着表达式需要 tuple 但 child_tuple_ 未设置
        LOG_WARN("ExpressionTuple::get_value: child_tuple_ is nullptr and try_get_value failed. rc=%s, expr_type=%d", 
                 strrc(rc), (int)expression->type());
        return RC::INTERNAL;
      }
    }
    return rc;
  }

private:
  const vector<ExprPointerType> &expressions_;
  // 接收来自子算子next?得到的tuple
  const Tuple                   *child_tuple_ = nullptr;
};

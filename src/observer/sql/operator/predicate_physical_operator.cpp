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
// Created by WangYunlai on 2022/6/27.
//

#include "sql/operator/predicate_physical_operator.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/field/field.h"
#include "storage/record/record.h"
#include "sql/expr/tuple.h"

PredicatePhysicalOperator::PredicatePhysicalOperator(std::unique_ptr<Expression> expr) : expression_(std::move(expr))
{
  ASSERT(expression_->value_type() == AttrType::BOOLEANS, "predicate's expression should be BOOLEAN type");
}

RC PredicatePhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 1) {
    LOG_WARN("predicate operator must has one child");
    return RC::INTERNAL;
  }

  // 递归传递 outer_tuple 给子算子（用于相关子查询）
  if (outer_tuple != nullptr) {
    children_[0]->set_outer_tuple(outer_tuple);
  }
  return children_[0]->open(trx);
}

RC PredicatePhysicalOperator::next()
{
  RC                rc   = RC::SUCCESS;
  PhysicalOperator *oper = children_.front().get();

  while (RC::SUCCESS == (rc = oper->next())) {
    Tuple *tuple = oper->current_tuple();
    if (nullptr == tuple) {
      rc = RC::INTERNAL;
      LOG_WARN("failed to get tuple from operator");
      break;
    }

    Value value;
    // 对于相关子查询，需要将外层查询的 tuple 和子查询的 tuple 组合
    // 这样 WHERE 条件中的 FieldExpr 才能访问到外层查询的字段
    Tuple *eval_tuple = tuple;
    if (outer_tuple != nullptr) {
      // 组合外层查询和子查询的 tuple
      joined_tuple_.set_left(outer_tuple);
      joined_tuple_.set_right(tuple);
      eval_tuple = &joined_tuple_;
    }
    
    // 将形如age > 18的布尔表达式应用到该元组上 --> 获得bool的Value ?
    // expression_为ComparisonExpr
    rc = expression_->get_value(*eval_tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    
    // 如果为true，就不继续更改current_tuple的值
    if (value.get_boolean()) {
      return rc;
    }
  }
  return rc;
}

RC PredicatePhysicalOperator::close()
{
  // 始终会报SUCCESS ???
  children_[0]->close();
  return RC::SUCCESS;
}

Tuple *PredicatePhysicalOperator::current_tuple() { return children_[0]->current_tuple(); }

RC PredicatePhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  return children_[0]->tuple_schema(schema);
}

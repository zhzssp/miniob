#pragma once

#include "sql/operator/logical_operator.h"

class OrderByLogicalOperator : public LogicalOperator
{
public:
  // 使用右值引用，从而能够接收std::move传来的参数
  OrderByLogicalOperator(vector<unique_ptr<OrderedUnboundFieldExpr>> &&order_by_exprs);

  virtual ~OrderByLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }
  OpType              get_op_type() const override { return OpType::LOGICALORDERBY; }

  auto &order_by_expressions() { return order_by_expressions_; }

private:
  vector<unique_ptr<OrderedUnboundFieldExpr>> order_by_expressions_;
};

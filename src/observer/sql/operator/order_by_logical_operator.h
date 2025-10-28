#pragma once

#include "sql/operator/logical_operator.h"

class OrderByLogicalOperator : public LogicalOperator
{
public:
  OrderByLogicalOperator(vector<unique_ptr<OrderedUnboundExpr>> &&order_by_exprs);

  virtual ~OrderByLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }
  OpType              get_op_type() const override { return OpType::LOGICALORDERBY; }

  auto &order_by_expressions() { return order_by_expressions_; }

private:
  vector<unique_ptr<Expression>> order_by_expressions_;
};

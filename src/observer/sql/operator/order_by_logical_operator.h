#pragma once

#include "sql/operator/logical_operator.h"

class OrderByLogicalOperator : public LogicalOperator
{
public:
  // 使用右值引用，从而能够接收std::move传来的参数
  OrderByLogicalOperator(vector<unique_ptr<OrderedUnboundFieldExpr>> &&order_by_exprs);

  // vector的析构函数同时将其中的所有指针成员一同delete掉 ！！！
  ~OrderByLogicalOperator() {
    LOG_INFO("------------------------- Call of ~OrderByLogicalOperator -------------------------");
  }

  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }
  OpType              get_op_type() const override { return OpType::LOGICALORDERBY; }

  auto &order_by_expressions() { return order_by_expressions_; }

private:
  vector<unique_ptr<OrderedUnboundFieldExpr>> order_by_expressions_;
};

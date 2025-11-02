#include "common/log/log.h"
#include "sql/operator/order_by_logical_operator.h"
#include "sql/expr/expression.h"

using namespace std;

OrderByLogicalOperator::OrderByLogicalOperator(
    vector<unique_ptr<OrderedUnboundFieldExpr>> &&order_by_exprs)
{
  // 使用move避免调用拷贝构造
  order_by_expressions_  = std::move(order_by_exprs);
}
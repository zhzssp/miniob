#include "common/log/log.h"
#include "sql/operator/order_by_logical_operator.h"
#include "sql/expr/expression.h"

using namespace std;

OrderByLogicalOperator::OrderByLogicalOperator(
    vector<unique_ptr<OrderedUnboundExpr>> &&order_by_exprs)
{
  order_by_expressions_  = std::move(order_by_exprs);
}
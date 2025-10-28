#pragma once

#include "sql/expr/aggregate_hash_table.h"
#include "sql/operator/physical_operator.h"

/**
 * @brief Oroup By 物理算子(vectorized)
 * @ingroup PhysicalOperator
 */
class OrderByVecPhysicalOperator : public PhysicalOperator
{
public:
  OrderByVecPhysicalOperator(vector<OrderedUnboundFieldExpr *> &&order_by_exprs): order_by_expressions_(order_by_exprs) {};

  virtual ~OrderByVecPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::ORDER_BY_VEC; }

  RC open(Trx *trx) override { return RC::UNIMPLEMENTED; }
  RC next(Chunk &chunk) override { return RC::UNIMPLEMENTED; }
  RC close() override { return RC::UNIMPLEMENTED; }

private:
  vector<OrderedUnboundFieldExpr *> order_by_expressions_;
};
#pragma once

#include "common/lang/tuple.h"
#include "sql/operator/physical_operator.h"
#include "sql/expr/composite_tuple.h"
#include "sql/expr/expression.h"

/**
 * @brief Group By 物理算子基类
 * @ingroup PhysicalOperator
 */
class OrderByPhysicalOperator : public PhysicalOperator
{
public:
  // 命名的右值对象为左值 --> 也需要使用std::move()
  OrderByPhysicalOperator(vector<unique_ptr<OrderedUnboundFieldExpr>> &&expressions): index(-1){
    tuples_buffer.clear();
    order_by_expressions_ = std::move(expressions);
  }
  ~OrderByPhysicalOperator() {
    LOG_INFO("------------------------- Call of ~OrderByLogicalOperator -------------------------");
  }

protected:
  PhysicalOperatorType type() const override { return PhysicalOperatorType::ORDER_BY; }
  OpType               get_op_type() const override { return OpType::ORDER_BY; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;
  RC tuple_schema(TupleSchema &schema) const override;
  RC sort_buffer();

protected:
  // OrderedUnboundFieldExpr
  vector<unique_ptr<OrderedUnboundFieldExpr>> order_by_expressions_;
  int32_t index;
  vector<ValueListTuple *>      tuples_buffer;
};
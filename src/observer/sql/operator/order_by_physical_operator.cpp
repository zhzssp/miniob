#include "sql/operator/order_by_physical_operator.h"

""" order by 算子的下层应当只有一个直接相邻的算子？ """
""" open中先获取所有的tuples进行排序, 然后再在next中一步一步取出输出 """
RC OrderByLogicalOperator::open(Trx *trx) {
  RC rc = RC::SUCCESS;
  // order by函数也应当只有一个子算子
  if (children_.size() != 1) {
    LOG_WARN("order operator must has one child");
    return RC::INTERNAL;
  }

  unique_ptr<PhysicalOperator> oper = children_.front();

  while (RC::SUCCESS == (rc = oper->next())) {
    Tuple *tuple = oper->current_tuple();
    if (nullptr == tuple) {
      rc = RC::INTERNAL;
      LOG_WARN("Failed to get tuple from operator");
      return rc;
    }
    tuples_buffer.emplace_back(tuple);
  }

  rc = sort_buffer();
  rc = children_[0]->open(trx);

  return rc;
}

RC OrderByPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;
  // 第一次index + 1 = 0
  index++;
  return rc;
}

RC OrderByPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;
  tuples_buffer.clear();
  rc = children_[0]->close();
  return rc;
}

Tuple *OrderByPhysicalOperator::current_tuple() { return tuples_buffer[index]; }

RC OrderByPhysicalOperator::tuple_schema(TupleSchema &schema) const { return children_[0]->tuple_schema(schema); }

RC OrderByPhysicalOperator::sort_buffer() {
  std::sort(tuples_buffer.begin(), tuples_buffer.end(), [this](ValueListTuple *a, ValueListTuple *b) {
      // 遍历 expressions，根据每个排序字段进行排序
      for (const auto &expr : order_by_expressions_) {
        // ValueListTuple --> vector<Value> cells
        RC rc = RC::SUCCESS;
        Value value_a, value_b;
        rc = a->nonstrict_get_value(expr->table_name(), expr->field_name(), value_a);
        if(rc != RC::SUCCESS) {
          LOG_WARN("Cannot get value_a, return true defaultly");
          return true;
        }
        rc = b->nonstrict_get_value(expr->table_name(), expr->field_name(), value_b);
        if (rc != RC::SUCCESS) {
          LOG_WARN("Cannot get value_b, return true defaultly");
          return true;
        }

        // 比较字段的值，根据 ASC 或 DESC 排序
        int compare_result = value_a.compare(value_b);
        // a > b
        if (compare_result == 1) {
          // 如果排序方式是降序 --> a在b前 
          if (expr->order() == -1) {
            return true;  // 返回 true 表示 a 排在 b 前
          } else {
            return false;
          }
        }
        // a < b
        else if(compare_result == -1){
          if (expr->order() == -1) {
            return true;  // 返回 true 表示 a 排在 b 前
          } else {
            return false;
          }
        }
        // compare_result = 0
        else {
          return false;  // 如果所有字段值相同，保持原有顺序
        }
      }
    }
  );
  return RC::SUCCESS;
}
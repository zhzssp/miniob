#include "sql/operator/order_by_physical_operator.h"

/* order by 算子的下层应当只有一个直接相邻的算子？ 
 open中先获取所有的tuples进行排序, 然后再在next中一步一步取出输出 */
RC OrderByPhysicalOperator::open(Trx *trx) {
  LOG_INFO("Execute order by physical operator's open method");
  RC rc = RC::SUCCESS;
  // order by函数也应当只有一个子算子
  if (children_.size() != 1) {
    LOG_WARN("order operator must has one child");
    return RC::INTERNAL;
  }

  // unique_ptr的拷贝构造被禁用，使用get获取原始指针（unique_ptr自动管理生命周期）
  PhysicalOperator *oper = children_.front().get();
  rc = oper->open(trx);
  // 得到table_scan，算子顺序出问题了 ？
  while (RC::SUCCESS == (rc = oper->next()))
  {
    Tuple *tuple = oper->current_tuple();
    if (nullptr == tuple) {
      rc = RC::INTERNAL;
      LOG_WARN("Failed to get tuple from operator");
      return rc;
    }

    ValueListTuple *value_list_tuple = new ValueListTuple(); 
    rc = ValueListTuple::make(*tuple, *value_list_tuple);
    if(rc != RC::SUCCESS) {
      return rc;
    }
    if(value_list_tuple != nullptr) {
      tuples_buffer.emplace_back(value_list_tuple);
    }
    LOG_INFO("pushed tuple ptr=%p, tuples_buffer.size=%zu", value_list_tuple, tuples_buffer.size());
  }

  LOG_INFO("Building tuples_buffer done !");

  rc = sort_buffer();

  LOG_INFO("tuples_buffer is sorted");

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
  LOG_INFO("Execute order by physical operator's close method");
  RC rc = RC::SUCCESS;
  tuples_buffer.clear();
  rc = children_[0]->close();
  return rc;
}

Tuple *OrderByPhysicalOperator::current_tuple() { 
  LOG_INFO("order by physical operator's current_tuple()");
  if(index >= tuples_buffer.size()) {
    LOG_WARN("Index = %d hit the edge of tuples_buffer size %d, return nullptr", index, tuples_buffer.size());
    // 似乎会导致程序崩溃 ？？？
    return nullptr;
  }
  return tuples_buffer[index];
}

RC OrderByPhysicalOperator::tuple_schema(TupleSchema &schema) const { return children_[0]->tuple_schema(schema); }

RC OrderByPhysicalOperator::sort_buffer() {
  std::sort(tuples_buffer.begin(), tuples_buffer.end(), [this](ValueListTuple *a, ValueListTuple *b) {
      // 遍历 expressions，根据每个排序字段进行排序 --> order_by_expressions生命周期管理存在问题
      for (const auto &expr : order_by_expressions_) {
        if(expr == nullptr) {
          LOG_ERROR("Get null order_by_expression, return true defaultly");
          return true;
        }
        // ValueListTuple --> vector<Value> cells
        RC rc = RC::SUCCESS;
        Value value_a, value_b;

        // 当order by的字段不是表达为table.field时，table = null --> 在此时会引发搜索错误 ！！！
        LOG_INFO("Try to get value_a, table = %s, field = %s", expr->table_name(), expr->field_name());
        rc = a->nonstrict_get_value(expr->field_name(), value_a);
        if(rc != RC::SUCCESS) {
          LOG_WARN("Cannot get value_a, return true defaultly");
          return true;
        }
        LOG_INFO("Successfully get value_a = %s !!!", value_a.to_string().c_str());
        
        LOG_INFO("Try to get value_b, table = %s, field = %s", expr->table_name(), expr->field_name());
        rc = b->nonstrict_get_value(expr->field_name(), value_b);
        if (rc != RC::SUCCESS) {
          LOG_WARN("Cannot get value_b, return true defaultly");
          return true;
        }
        LOG_INFO("Successfully get value_b = %s !!!", value_b.to_string().c_str());

        // 比较字段的值，根据 ASC 或 DESC 排序
        int compare_result = value_a.compare(value_b);
        LOG_INFO("Get compare_result = %d, order = %d", compare_result, expr->order());
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
            return false;  
          } else {
            return true;
          }
        }
        // compare_result = 0
        else {
          LOG_INFO("Compare_result is 0, continue to compare the next field !");
          continue;
        }
      }
      // 最终的默认return --> 过编译检查
      return true;
    }
  );
  return RC::SUCCESS;
}
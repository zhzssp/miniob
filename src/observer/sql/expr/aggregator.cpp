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
// Created by Wangyunlai on 2024/05/29.
//

#include "sql/expr/aggregator.h"
#include "common/log/log.h"

RC SumAggregator::accumulate(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  // left, right, result
  if(!value.is_null()) {
    Value::add(value, value_, value_);
    not_null_count_++;
  }
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  if(not_null_count_ == 0) {
    result = Value("NULL", 4);
  } else {
    result = value_;
  }
  return RC::SUCCESS;
}

RC AvgAggregator::average(const Value &value)
{
  // AVG 聚合函数：累积值的总和和计数
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    if(!value.is_null()) {
      count_ = 1;
    } else {
      count_ = 0;
    }
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  // 累积值的总和
  if(!value.is_null()) {
    Value::add(value, value_, value_);
    count_++;
  }
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value& result)
{
  // 计算平均值：总和 / 计数
  if (count_ == 0) {
    // 如果没有值，返回0或NULL（这里返回NULL）
    result = Value("NULL", 4);
    return RC::SUCCESS;
  }
  
  // 将总和转换为float，然后除以计数
  // value_ 可能是 INT 或 FLOAT 类型，需要正确转换
  float sum_float;
  if (value_.attr_type() == AttrType::INTS) {
    sum_float = static_cast<float>(value_.get_int());
  } else if (value_.attr_type() == AttrType::FLOATS) {
    sum_float = value_.get_float();
  } else {
    LOG_WARN("unsupported value type for AVG: %s", attr_type_to_string(value_.attr_type()));
    return RC::UNIMPLEMENTED;
  }
  
  float avg = sum_float / static_cast<float>(count_);
  result = Value(avg);
  return RC::SUCCESS;
}

RC CountAggregator::count(const Value &value)
{
  // COUNT 聚合函数：每次调用递增计数
  // 对于 COUNT(*) 或 COUNT(expr)，每次都会传入一个值（通常是1，或expr的值）
  // 我们需要累积这些值来计算总数
  
  if (value_.attr_type() == AttrType::UNDEFINED) {
    // 第一次调用：初始化计数，通常传入的值是1（对于COUNT(*)）或expr的值（对于COUNT(expr)）
    if(value.is_null()) {
      value_ = Value(0);
    } else {
      value_ = Value(1);
    }
    return RC::SUCCESS;
  }
  
  // 后续调用：将传入的值加到总数上
  // 对于 COUNT(*)，value 通常是1；对于 COUNT(expr)，value 可能是 expr 的值（需要处理NULL）
  Value one(1);  // 对于COUNT，每次应该加1，但为了兼容性，我们使用传入的值
  // 实际上，COUNT(expr) 应该检查 expr 是否为 NULL，如果不为 NULL 才计数
  
  if(!value.is_null()) {
    Value::add(one, value_, value_);
    LOG_TRACE("Count + 1, cause that value is not null");
  }
  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value& result)
{
  // 如果从未调用过count()（即没有数据行），应该返回0
  if (value_.attr_type() == AttrType::UNDEFINED) {
    result = Value(0);
    return RC::SUCCESS;
  }
  result = value_;
  return RC::SUCCESS;
}

RC MaxAggregator::max(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    count_++;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  if (value.compare(value_) > 0) {
    value_ = value;
    count_++;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::evaluate(Value& result)
{
  if(count_ == 0) {
    value_ = Value("NULL", 4);
    value_.set_null(true);
  }
  result = value_;
  return RC::SUCCESS;
}

RC MinAggregator::min(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    count_++;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  if (value.compare(value_) < 0) {
    value_ = value;
    count_++;
  }
  return RC::SUCCESS;
}

RC MinAggregator::evaluate(Value& result)
{
  if (count_ == 0) {
    value_ = Value("NULL", 4);
    value_.set_null(true);
  }
  result = value_;
  return RC::SUCCESS;
}
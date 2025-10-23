/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/physical_operator.h"
#include "sql/parser/parse.h"
#include "sql/expr/expression.h"
#include <unordered_map>
#include <vector>
#include <memory>

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 * @details 基于哈希表的连接算法，适用于等值连接
 */

class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator();
  virtual ~HashJoinPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }
  
  OpType get_op_type() const override { return OpType::INNERHASHJOIN; }

  virtual double calculate_cost(
      LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override
  {
    return 0.0;
  }

  RC     open(Trx *trx) override;
  RC     next() override;
  RC     close() override;
  Tuple *current_tuple() override;

  // 设置 JOIN 字段表达式
  void set_join_fields(FieldExpr *left_field, FieldExpr *right_field);
  
  // 设置额外的过滤条件
  void set_filter_expressions(const vector<unique_ptr<Expression>> &expressions);

private:
  // 构建哈希表（使用右表）
  RC build_hash_table();
  
  // 探测哈希表（使用左表）
  RC probe_hash_table();
  
  // 获取下一个匹配的右表记录
  RC get_next_match();
  
  // 物化 Tuple 数据到 ValueListTuple
  unique_ptr<ValueListTuple> materialize_tuple(Tuple *tuple);
  
  // 从 Tuple 中获取字段值
  RC get_field_value(const Tuple &tuple, const Field &field, Value &value);
  
  // 评估过滤条件
  bool evaluate_filter_conditions();

  Trx *trx_ = nullptr;
  
  // 子操作符
  PhysicalOperator *left_  = nullptr;   // 左表操作符
  PhysicalOperator *right_ = nullptr;   // 右表操作符
  
  // 当前状态
  Tuple *left_tuple_  = nullptr;        // 当前左表记录
  Tuple *right_tuple_ = nullptr;      // 当前右表记录
  JoinedTuple joined_tuple_;           // 连接后的记录
  
  // JOIN 字段信息
  Field left_join_field_;   // 左表 JOIN 字段
  Field right_join_field_;  // 右表 JOIN 字段
  
  // 哈希表相关
  unordered_map<string, vector<unique_ptr<ValueListTuple>>> hash_table_;  // 哈希表，存储物化的右表记录
  vector<unique_ptr<ValueListTuple>>* current_matches_ = nullptr;        // 当前匹配的右表记录（指向哈希表中的数据）
  size_t current_match_index_ = 0;                                       // 当前匹配记录索引
  
  // 过滤条件
  vector<unique_ptr<Expression>> filter_expressions_;
  
  // 状态标志
  bool hash_table_built_ = false;       // 哈希表是否已构建
  bool left_exhausted_ = false;         // 左表是否已遍历完
  bool right_exhausted_ = false;        // 右表是否已遍历完
};
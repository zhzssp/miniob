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
// Created by Assistant on 2024/01/20.
//

#include "sql/operator/hash_join_physical_operator.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "storage/trx/trx.h"

using namespace std;

HashJoinPhysicalOperator::HashJoinPhysicalOperator()
{
  // 构造函数中不初始化子操作符，在 open() 中初始化
}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("HashJoinPhysicalOperator should have exactly 2 children");
    return RC::INTERNAL;
  }

  trx_ = trx;
  left_  = children_[0].get();
  right_ = children_[1].get();

  // 打开子操作符
  RC rc = left_->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open left child operator: %s", strrc(rc));
    return rc;
  }

  rc = right_->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open right child operator: %s", strrc(rc));
    return rc;
  }

  // 构建哈希表
  rc = build_hash_table();
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to build hash table: %s", strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::next()
{
  if (left_exhausted_) {
    return RC::RECORD_EOF;
  }

  // 检查是否使用嵌套循环行为（没有等值条件）
  if (left_join_field_.meta() == nullptr) {
    // 嵌套循环行为：遍历左表和右表的所有组合
    while (true) {
      RC rc = RC::SUCCESS;
      
      // 获取下一个左表记录
      if (left_tuple_ == nullptr) {
        rc = left_->next();
        if (rc != RC::SUCCESS) {
          if (rc == RC::RECORD_EOF) {
            left_exhausted_ = true;
            return RC::RECORD_EOF;
          }
          return rc;
        }
        left_tuple_ = left_->current_tuple();
        joined_tuple_.set_left(left_tuple_);
      }
      
      // 获取下一个右表记录
      rc = right_->next();
      if (rc != RC::SUCCESS) {
        if (rc == RC::RECORD_EOF) {
          // 右表遍历完，重置左表和右表
          left_tuple_ = nullptr;
          right_->close();
          right_->open(trx_);
          continue; // 继续循环获取下一个左表记录
        }
        return rc;
      }
      
      right_tuple_ = right_->current_tuple();
      joined_tuple_.set_right(right_tuple_);
      
      // 应用过滤条件
      if (!filter_expressions_.empty()) {
        if (!evaluate_filter_conditions()) {
          // 当前记录不满足过滤条件，继续下一个匹配
          continue;
        }
      }
      
      return RC::SUCCESS;
    }
  }

  // 正常的 Hash Join 行为
  // 如果当前没有匹配的记录，尝试获取下一个左表记录
  if (current_matches_ == nullptr || current_match_index_ >= current_matches_->size()) {
    RC rc = probe_hash_table();
    if (rc != RC::SUCCESS) {
      if (rc == RC::RECORD_EOF) {
        left_exhausted_ = true;
        return RC::RECORD_EOF;
      }
      return rc;
    }
    
    // 如果仍然没有匹配，继续下一个左表记录
    if (current_matches_ == nullptr || current_matches_->empty()) {
      // 使用循环而不是递归调用，避免无限循环
      while (current_matches_ == nullptr || current_matches_->empty()) {
        rc = probe_hash_table();
        if (rc != RC::SUCCESS) {
          if (rc == RC::RECORD_EOF) {
            left_exhausted_ = true;
            return RC::RECORD_EOF;
          }
          return rc;
        }
        // 如果找到了匹配，退出循环
        if (current_matches_ != nullptr && !current_matches_->empty()) {
          break;
        }
      }
    }
  }

  // 获取下一个匹配的右表记录
  RC rc = get_next_match();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 应用过滤条件
  if (!filter_expressions_.empty()) {
    if (!evaluate_filter_conditions()) {
      // 当前记录不满足过滤条件，继续下一个匹配
      return next();
    }
  }

  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::close()
{
  // 清理哈希表
  hash_table_.clear();
  current_matches_ = nullptr;
  
  // 重置状态
  hash_table_built_ = false;
  left_exhausted_ = false;
  right_exhausted_ = false;
  current_match_index_ = 0;

  // 关闭子操作符
  if (left_ != nullptr) {
    left_->close();
  }
  if (right_ != nullptr) {
    right_->close();
  }

  return RC::SUCCESS;
}

Tuple *HashJoinPhysicalOperator::current_tuple()
{
  return &joined_tuple_;
}

void HashJoinPhysicalOperator::set_join_fields(FieldExpr *left_field, FieldExpr *right_field)
{
  // 复制字段信息而不是存储指针
  left_join_field_ = left_field->field();
  right_join_field_ = right_field->field();
}

void HashJoinPhysicalOperator::set_filter_expressions(const vector<unique_ptr<Expression>> &expressions)
{
  // 复制过滤条件
  for (const auto &expr : expressions) {
    filter_expressions_.push_back(expr->copy());
  }
  //LOG_INFO("HashJoinPhysicalOperator: Set %zu filter expressions", filter_expressions_.size());
}

unique_ptr<ValueListTuple> HashJoinPhysicalOperator::materialize_tuple(Tuple *tuple)
{
  if (tuple == nullptr) {
    return nullptr;
  }

  auto materialized_tuple = make_unique<ValueListTuple>();
  
  // 提取所有单元格数据
  vector<Value> cells;
  vector<TupleCellSpec> specs;
  
  int cell_count = tuple->cell_num();
  for (int i = 0; i < cell_count; i++) {
    Value cell;
    RC rc = tuple->cell_at(i, cell);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to get cell at index %d: %s", i, strrc(rc));
      continue;
    }
    cells.push_back(cell);
    
    // 获取单元格规范
    TupleCellSpec spec;
    rc = tuple->spec_at(i, spec);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to get spec at index %d: %s", i, strrc(rc));
      // 创建一个默认的规范
      spec = TupleCellSpec();
    }
    specs.push_back(spec);
  }
  
  materialized_tuple->set_cells(cells);
  materialized_tuple->set_names(specs);
  
  return materialized_tuple;
}

RC HashJoinPhysicalOperator::get_field_value(const Tuple &tuple, const Field &field, Value &value)
{
  // 从 Field 创建 TupleCellSpec，然后从 Tuple 中获取值
  TupleCellSpec spec(field.table_name(), field.field_name());
  
  // 检查 Field 对象的表信息
  if (field.table() == nullptr) {
    LOG_WARN("Field table is null for field %s", field.field_name());
    return RC::INVALID_ARGUMENT;
  }
  
  RC rc = tuple.find_cell(spec, value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("Failed to find field %s.%s in tuple: %s", field.table_name(), field.field_name(), strrc(rc));
  }
  
  return rc;
}

bool HashJoinPhysicalOperator::evaluate_filter_conditions()
{
  // 评估所有过滤条件，所有条件都必须为真
  for (const auto &expr : filter_expressions_) {
    Value result;
    RC rc = expr->get_value(joined_tuple_, result);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to evaluate filter condition: %s", strrc(rc));
      return false;
    }
    
    // 检查结果是否为真
    if (result.attr_type() == AttrType::UNDEFINED || !result.get_boolean()) {
      return false;
    }
  }
  
  return true;
}

RC HashJoinPhysicalOperator::build_hash_table()
{
  if (hash_table_built_) {
    return RC::SUCCESS;
  }

  if (right_join_field_.meta() == nullptr) {
    // 没有等值条件时，不构建哈希表，直接返回成功
    hash_table_built_ = true;
    return RC::SUCCESS;
  }
  
  // 遍历右表，构建哈希表
  RC rc = RC::SUCCESS;
  while (OB_SUCC(rc = right_->next())) {
    Tuple *right_tuple = right_->current_tuple();
    if (right_tuple == nullptr) {
      continue;
    }

    // 从右表记录中提取 JOIN 字段的值
    Value join_value;
    rc = get_field_value(*right_tuple, right_join_field_, join_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to get join field value from right tuple: %s", strrc(rc));
      continue;
    }

    // 将 Value 转换为字符串作为哈希键
    string hash_key = join_value.to_string();
    
    // 创建右表记录的副本（物化）
    auto right_tuple_copy = materialize_tuple(right_tuple);
    if (right_tuple_copy != nullptr) {
      hash_table_[hash_key].push_back(std::move(right_tuple_copy));
    }
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to build hash table: %s", strrc(rc));
    return rc;
  }

  hash_table_built_ = true;
  
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::probe_hash_table()
{
  if (left_join_field_.meta() == nullptr) {
    // 没有等值条件时，直接获取下一个右表记录
    RC rc = right_->next();
    if (rc != RC::SUCCESS) {
      if (rc == RC::RECORD_EOF) {
        right_exhausted_ = true;
        return RC::RECORD_EOF;
      }
      return rc;
    }
    
    right_tuple_ = right_->current_tuple();
    if (right_tuple_ == nullptr) {
      return RC::SUCCESS;
    }
    
    // 设置当前匹配为单个记录
    current_matches_ = nullptr; // 表示使用嵌套循环行为
    current_match_index_ = 0;
    return RC::SUCCESS;
  }

  // 获取下一个左表记录
  RC rc = left_->next();
  if (rc != RC::SUCCESS) {
    if (rc == RC::RECORD_EOF) {
      left_exhausted_ = true;
      return RC::RECORD_EOF;
    }
    return rc;
  }

  left_tuple_ = left_->current_tuple();
  if (left_tuple_ == nullptr) {
    // 如果左表记录为空，返回 SUCCESS 让上层处理
    return RC::SUCCESS;
  }

  // 从左表记录中提取 JOIN 字段的值
  Value join_value;
  rc = get_field_value(*left_tuple_, left_join_field_, join_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("Failed to get join field value from left tuple: %s", strrc(rc));
    // 如果无法获取字段值，返回 SUCCESS 让上层处理
    return RC::SUCCESS;
  }

  // 将 Value 转换为字符串作为哈希键
  string hash_key = join_value.to_string();
  
  // 在哈希表中查找匹配的记录
  auto it = hash_table_.find(hash_key);
  
  if (it != hash_table_.end() && !it->second.empty()) {
    // 直接使用哈希表中已经物化的记录，不需要再次复制
    current_matches_ = &(it->second);
    current_match_index_ = 0;
  } else {
    current_matches_ = nullptr;
    current_match_index_ = 0;
    // 如果没有匹配，返回 SUCCESS 让上层处理
    return RC::SUCCESS;
  }

  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::get_next_match()
{
  if (current_matches_ == nullptr || current_match_index_ >= current_matches_->size()) {
    // 当前左表记录的所有匹配都已处理完，返回 SUCCESS 让上层处理
    return RC::SUCCESS;
  }

  // 获取当前匹配的右表记录
  right_tuple_ = (*current_matches_)[current_match_index_].get();
  current_match_index_++;

  // 创建连接后的记录
  joined_tuple_.set_left(left_tuple_);
  joined_tuple_.set_right(right_tuple_);

  return RC::SUCCESS;
}
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
// Created by WangYunlai on 2022/07/01.
//

#include "sql/operator/project_physical_operator.h"
#include "common/log/log.h"
#include "storage/record/record.h"
#include "storage/table/table.h"

using namespace std;

ProjectPhysicalOperator::ProjectPhysicalOperator(vector<unique_ptr<Expression>> &&expressions)
  : expressions_(std::move(expressions)), tuple_(expressions_)
{
}

RC ProjectPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  PhysicalOperator *child = children_[0].get();
  RC                rc    = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}

RC ProjectPhysicalOperator::next()
{
  if (children_.empty()) {
    return RC::RECORD_EOF;
  }
  return children_[0]->next();
}

RC ProjectPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}

Tuple *ProjectPhysicalOperator::current_tuple()
{
  // 从下层算子获取其得到的tuple
  LOG_INFO("project physical operator's current_tuple");
  Tuple *new_tuple = children_[0]->current_tuple();
  if(new_tuple == nullptr) {
    LOG_WARN("Get null tuple from child[0]");
    return nullptr;
  }
  LOG_WARN("ProjectPhysicalOperator::current_tuple: setting child_tuple_=%p, cell_num=%d", 
           new_tuple, new_tuple ? new_tuple->cell_num() : -1);
  tuple_.set_tuple(new_tuple);
  return &tuple_;
}

RC ProjectPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  // expressions_存的是select选中的字段
  for (const unique_ptr<Expression> &expression : expressions_) {
    if (expression->has_alias()) {
      // 如果有别名，使用别名
      schema.append_cell(expression->alias());
    } else {
      // 如果没有别名，使用原始名称
      schema.append_cell(expression->name());
    }
  }
  return RC::SUCCESS;
}
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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/filter_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/sys/rc.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/expr/subquery_expr.h"

FilterStmt::~FilterStmt()
{
  for (FilterUnit *unit : filter_units_) {
    // 释放 FilterUnit 中的 Expression* 内存
    if (unit->left().is_expr && unit->left().expression != nullptr) {
      delete unit->left().expression;
    }
    if (unit->right().is_expr && unit->right().expression != nullptr) {
      delete unit->right().expression;
    }
    delete unit;
  }
  filter_units_.clear();
}

RC FilterStmt::create(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const ConditionSqlNode *conditions, int condition_num, FilterStmt *&stmt)
{
  RC rc = RC::SUCCESS;
  stmt  = nullptr;

  FilterStmt *tmp_stmt = new FilterStmt();
  for (int i = 0; i < condition_num; i++) {
    FilterUnit *filter_unit = nullptr;

    rc = create_filter_unit(db, default_table, tables, conditions[i], filter_unit);
    if (rc != RC::SUCCESS) {
      delete tmp_stmt;
      LOG_WARN("failed to create filter unit. condition index=%d", i);
      return rc;
    }
    tmp_stmt->filter_units_.push_back(filter_unit);
  }

  stmt = tmp_stmt;
  return rc;
}

RC get_table_and_field(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const RelAttrSqlNode &attr, Table *&table, const FieldMeta *&field)
{
  
  if (common::is_blank(attr.relation_name.c_str())) {
    table = default_table;
  } else if (nullptr != tables) {
    auto iter = tables->find(attr.relation_name);
    if (iter != tables->end()) {
      table = iter->second;
    }
  } else {
    table = db->find_table(attr.relation_name.c_str());
  }
  if (nullptr == table) {
    LOG_WARN("No such table: attr.relation_name: %s", attr.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  field = table->table_meta().field(attr.attribute_name.c_str());
  if (nullptr == field) {
    LOG_WARN("no such field in table: table %s, field %s", table->name(), attr.attribute_name.c_str());
    table = nullptr;
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  return RC::SUCCESS;
}

RC FilterStmt::create_filter_unit(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const ConditionSqlNode &condition, FilterUnit *&filter_unit)
{
  RC rc = RC::SUCCESS;

  CompOp comp = condition.comp;
  if (comp < EQUAL_TO || comp > IS_NOT_OP) {
    LOG_WARN("invalid compare operator : %d", comp);
    return RC::INVALID_ARGUMENT;
  }

  filter_unit = new FilterUnit;
  
  LOG_WARN("Creating FilterUnit: left_is_attr=%d, left_expr=%p, right_is_attr=%d, right_expr=%p, comp=%d", 
           condition.left_is_attr, condition.left_expr, condition.right_is_attr, condition.right_expr, condition.comp);

  // 左边是字段
  if (condition.left_is_attr) {
    Table           *table = nullptr;
    const FieldMeta *field = nullptr;
    // 获取table和field meta
    rc                     = get_table_and_field(db, default_table, tables, condition.left_attr, table, field);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot find attr");
      return rc;
    }
    FilterObj filter_obj;
    // 以字段的方式进行初始化
    filter_obj.init_attr(Field(table, field));
    filter_unit->set_left(filter_obj);
  } else if (condition.left_expr != nullptr) {
    FilterObj filter_obj;
    LOG_WARN("[FilterStmt] Creating left filter_obj from expression, expr=%p, expr_type=%d", condition.left_expr, (int)condition.left_expr->type());
    filter_obj.init_expression(condition.left_expr->copy().release());
    filter_unit->set_left(filter_obj);
  } else {
    FilterObj filter_obj;
    // 左边是值 --> 直接以值的方式进行初始化
    filter_obj.init_value(condition.left_value);
    filter_unit->set_left(filter_obj);
  }

  // 右边同理
  if (condition.right_is_attr) {
    Table           *table = nullptr;
    const FieldMeta *field = nullptr;
    rc                     = get_table_and_field(db, default_table, tables, condition.right_attr, table, field);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot find attr");
      return rc;
    }
    FilterObj filter_obj;
    filter_obj.init_attr(Field(table, field));
    filter_unit->set_right(filter_obj);
  } else if (condition.right_expr != nullptr) {
    FilterObj filter_obj;
    LOG_WARN("[FilterStmt] Creating right filter_obj from expression, expr=%p, expr_type=%d", condition.right_expr, (int)condition.right_expr->type());
    filter_obj.init_expression(condition.right_expr->copy().release());
    filter_unit->set_right(filter_obj);
  } else {
    LOG_WARN("Creating FilterObj from condition.right_value");
    FilterObj filter_obj;
    filter_obj.init_value(condition.right_value);
    filter_unit->set_right(filter_obj);
  }

  // 设置比较运算符
  filter_unit->set_comp(comp);

  // 检查两个类型是否能够比较
  return rc;
}

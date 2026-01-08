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

#include "sql/stmt/update_stmt.h"
#include "sql/expr/subquery_expr.h"
#include "sql/parser/expression_binder.h"
#include "sql/stmt/select_stmt.h"

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  // 检查表是否存在
  Table *table = db->find_table(update_sql.relation_name.c_str());
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), update_sql.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta &table_meta = table->table_meta();
  vector<UpdateFieldAssignment> assignments;

  // 如果使用新的 assignments 列表
  if (!update_sql.assignments.empty()) {
    for (const auto &assign : update_sql.assignments) {
      // 检查字段是否存在
      const FieldMeta *field_meta = table_meta.field(assign.attribute_name.c_str());
      if (nullptr == field_meta) {
        LOG_WARN("no such field. field=%s.%s.%s", db->name(), table->name(), assign.attribute_name.c_str());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }

      // 复制表达式
      if (assign.value_expr == nullptr) {
        LOG_WARN("update value expression is null for field %s", assign.attribute_name.c_str());
        return RC::INVALID_ARGUMENT;
      }
      unique_ptr<Expression> value_expr = assign.value_expr->copy();

      // 处理子查询表达式
      if (value_expr->type() == ExprType::SUB_QUERY) {
        SubqueryExpr *subquery_expr = static_cast<SubqueryExpr *>(value_expr.get());
        
        // 创建子查询的 Stmt
        Stmt *sub_stmt = nullptr;
        RC rc = SelectStmt::create(
          db,
          subquery_expr->sub_query_sn()->selection,
          sub_stmt,
          nullptr,  // name2alias
          nullptr,  // alias2name
          nullptr,    // loaded_relation_names (UPDATE 语句中，子查询不能访问外层表)
          nullptr     // field_alias2name
        );
        if (rc != RC::SUCCESS) {
          LOG_WARN("cannot construct subquery stmt. rc=%s", strrc(rc));
          return rc;
        }

        // 检查子查询的合法性：子查询的查询的属性只能有一个
        RC rc_ = Stmt::check_sub_select_legal(db, subquery_expr->sub_query_sn());
        if (rc_ != RC::SUCCESS) {
          LOG_WARN("subquery is not legal. rc=%s", strrc(rc_));
          delete sub_stmt;
          return rc_;
        }
        
        subquery_expr->set_stmt(unique_ptr<SelectStmt>(static_cast<SelectStmt *>(sub_stmt)));
      }

      UpdateFieldAssignment field_assign;
      field_assign.attribute_name = assign.attribute_name;
      field_assign.value_expr = std::move(value_expr);
      assignments.push_back(std::move(field_assign));
    }
  } else {
    // 向后兼容：使用单个字段和表达式
    const FieldMeta *field_meta = table_meta.field(update_sql.attribute_name.c_str());
    if (nullptr == field_meta) {
      LOG_WARN("no such field. field=%s.%s.%s", db->name(), table->name(), update_sql.attribute_name.c_str());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }

    if (update_sql.value_expr == nullptr) {
      LOG_WARN("update value expression is null");
      return RC::INVALID_ARGUMENT;
    }
    unique_ptr<Expression> value_expr = update_sql.value_expr->copy();

    // 处理子查询表达式
    if (value_expr->type() == ExprType::SUB_QUERY) {
      SubqueryExpr *subquery_expr = static_cast<SubqueryExpr *>(value_expr.get());
      
      Stmt *sub_stmt = nullptr;
      RC rc = SelectStmt::create(
        db,
        subquery_expr->sub_query_sn()->selection,
        sub_stmt,
        nullptr, nullptr, nullptr, nullptr
      );
      if (rc != RC::SUCCESS) {
        LOG_WARN("cannot construct subquery stmt. rc=%s", strrc(rc));
        return rc;
      }

      RC rc_ = Stmt::check_sub_select_legal(db, subquery_expr->sub_query_sn());
      if (rc_ != RC::SUCCESS) {
        LOG_WARN("subquery is not legal. rc=%s", strrc(rc_));
        delete sub_stmt;
        return rc_;
      }
      
      subquery_expr->set_stmt(unique_ptr<SelectStmt>(static_cast<SelectStmt *>(sub_stmt)));
    }

    UpdateFieldAssignment field_assign;
    field_assign.attribute_name = update_sql.attribute_name;
    field_assign.value_expr = std::move(value_expr);
    assignments.push_back(std::move(field_assign));
  }

  // 解析WHERE条件
  FilterStmt *filter_stmt = nullptr;
  RC rc = RC::SUCCESS;
  const char *table_name = update_sql.relation_name.c_str();
  unordered_map<string, Table *> table_map;
  table_map.insert(pair<string, Table *>(string(table_name), table));
  if (!update_sql.conditions.empty()) {
    rc = FilterStmt::create(db, table, &table_map, update_sql.conditions.data(),
                           static_cast<int>(update_sql.conditions.size()), filter_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create filter statement. rc=%s", strrc(rc));
      return rc;
    }
  }

  stmt = new UpdateStmt(table, std::move(assignments), filter_stmt);

  return RC::SUCCESS;
}
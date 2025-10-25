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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"

using namespace std;
using namespace common;

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
  
  if (nullptr != join_filter_stmt_) {
    delete join_filter_stmt_;
    join_filter_stmt_ = nullptr;
  }
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  BinderContext binder_context;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  
  // 
  unordered_map<string, string> table_alias_map;  // 别名 -> 表名映射
  unordered_map<string, string> field_alias_map;  // 字段别名映射

  // 处理表别名 - 优先处理 ALIASES，避免重复
  if (!select_sql.ALIASES.empty()) {
    // 多表查询（逗号分隔），使用 ALIASES
    for (size_t i = 0; i < select_sql.ALIASES.size(); i++) {
      if (!select_sql.ALIASES[i].alias.empty()) {
          // 检查别名重复
          if (table_alias_map.find(select_sql.ALIASES[i].alias) != table_alias_map.end()) {
              LOG_WARN("Duplicate table alias: %s", select_sql.ALIASES[i].alias.c_str());
              return RC::INVALID_ARGUMENT;
          }
          table_alias_map[select_sql.ALIASES[i].alias] = select_sql.ALIASES[i].name;
      }
    }
  } else {
    // JOIN查询，使用 table_references
    for (const auto &table_ref : select_sql.table_references) {
      if (!table_ref.alias.empty()) {
          // 检查别名重复
          if (table_alias_map.find(table_ref.alias) != table_alias_map.end()) {
              LOG_WARN("Duplicate table alias: %s", table_ref.alias.c_str());
              return RC::INVALID_ARGUMENT;
          }
          table_alias_map[table_ref.alias] = table_ref.table_name;
      }
    }
  }
// 处理字段别名
  for (size_t i = 0; i < select_sql.expressions.size(); i++) {
      Expression *expr = select_sql.expressions[i].get();
      if (expr->has_alias()) {
          field_alias_map[expr->alias()] = expr->name();
      }
  }

  // 处理传统的 relations（向后兼容）
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const char *table_name = select_sql.relations[i].c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
  }
  
  // 处理 ALIASES（多表查询的表别名）
  for (size_t i = 0; i < select_sql.ALIASES.size(); i++) {
    const char *table_name = select_sql.ALIASES[i].name.c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    // 如果表还没有被添加，则添加它
    if (table_map.find(table_name) == table_map.end()) {
      binder_context.add_table(table);
      tables.push_back(table);
      table_map.insert({table_name, table});
    }
    
    // 如果有别名，添加到table_map中
    if (!select_sql.ALIASES[i].alias.empty()) {
      table_map.insert({select_sql.ALIASES[i].alias, table});
      binder_context.add_table_alias(select_sql.ALIASES[i].alias.c_str(), table);
    }
  }
  
  // 处理新的 table_references（支持 JOIN）
  for (const auto &table_ref : select_sql.table_references) {
    const char *table_name = table_ref.table_name.c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. table name is null");
      return RC::INVALID_ARGUMENT;
    }

    // 检查表是否已经存在，避免重复添加
    if (table_map.find(table_name) != table_map.end()) {
      //LOG_INFO("Table %s already exists, skipping duplicate", table_name);
      continue;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

     binder_context.add_table(table);
     tables.push_back(table);
     table_map.insert({table_name, table});
     
     // 如果有别名，也要添加到table_map中
     if (!table_ref.alias.empty()) {
       table_map.insert({table_ref.alias, table});
       binder_context.add_table_alias(table_ref.alias.c_str(), table);
     }
     
     // 处理 JOIN 条件
     if (table_ref.is_join && !table_ref.join_conditions.empty()) {
     }
  }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
  // 在绑定表达式之前，先解析表别名引用
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
     // 如果是 UnboundFieldExpr，需要解析表别名
     if (expression->type() == ExprType::UNBOUND_FIELD) {
       UnboundFieldExpr *field_expr = static_cast<UnboundFieldExpr*>(expression.get());
       if (field_expr->table_name() != nullptr && strlen(field_expr->table_name()) > 0) {
         // 检查是否是表别名
         auto it = table_alias_map.find(field_expr->table_name());
         if (it != table_alias_map.end()) {
           // 将表别名转换为实际表名
           field_expr->set_table_name(it->second.c_str());
         }
       }
     }
    
    RC rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  vector<unique_ptr<Expression>> group_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.group_by) {
    RC rc = expression_binder.bind_expression(expression, group_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  // create filter statement in `where` statement
  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(db,
      default_table,
      &table_map,
      select_sql.conditions.data(),
      static_cast<int>(select_sql.conditions.size()),
      filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  // 处理 JOIN 条件
  FilterStmt *join_filter_stmt = nullptr;
  vector<FilterStmt*> table_join_filters; // 临时存储，稍后设置到 SelectStmt 对象中
  
  if (!select_sql.table_references.empty()) {
    // 收集所有 JOIN 条件到一个全局的 FilterStmt 中
    vector<ConditionSqlNode> all_join_conditions;
    for (const auto &table_ref : select_sql.table_references) {
      if (table_ref.is_join && !table_ref.join_conditions.empty()) {
        for (const auto &join_cond : table_ref.join_conditions) {
          ConditionSqlNode condition;
          condition.left_is_attr = join_cond.left_is_attr;
          condition.left_attr = join_cond.left_attr;
          condition.left_value = join_cond.left_value;
          condition.right_is_attr = join_cond.right_is_attr;
          condition.right_attr = join_cond.right_attr;
          condition.right_value = join_cond.right_value;
          condition.comp = join_cond.comp;
          
          // 转换JOIN条件中的表别名
          if (condition.left_is_attr && !condition.left_attr.relation_name.empty()) {
            auto it = table_alias_map.find(condition.left_attr.relation_name);
            if (it != table_alias_map.end()) {
              condition.left_attr.relation_name = it->second;
            }
          }
          if (condition.right_is_attr && !condition.right_attr.relation_name.empty()) {
            auto it = table_alias_map.find(condition.right_attr.relation_name);
            if (it != table_alias_map.end()) {
              condition.right_attr.relation_name = it->second;
            }
          }
          
          all_join_conditions.push_back(condition);
        }
      }
    }
    
    // 创建全局的 JOIN 条件过滤器
    if (!all_join_conditions.empty()) {
      rc = FilterStmt::create(db,
          default_table,
          &table_map,
          all_join_conditions.data(),
          static_cast<int>(all_join_conditions.size()),
          join_filter_stmt);
      if (rc != RC::SUCCESS) {
        LOG_WARN("cannot construct join filter stmt");
        return rc;
      }
    }
    
    // 为每个表创建空的 JOIN 条件过滤器（保持接口兼容性）
    for (size_t k = 0; k < select_sql.table_references.size(); k++) {
      table_join_filters.push_back(nullptr);
    }
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();

  select_stmt->tables_.swap(tables);
  select_stmt->table_join_filters_.swap(table_join_filters);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->join_filter_stmt_ = join_filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  stmt                      = select_stmt;
  return RC::SUCCESS;
}

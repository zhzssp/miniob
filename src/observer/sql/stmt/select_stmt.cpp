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


RC SelectStmt::convert_alias_to_name(Expression *expr, 
std::shared_ptr<std::unordered_map<string, string>> alias2name,
std::shared_ptr<std::unordered_map<string, string>> field_alias2name) {
  //  if (expr->type() == ExprType::VALUE || 
  //  expr->type() == ExprType::SUB_QUERY || 
  //  expr->type() == ExprType::SPECIAL_PLACEHOLDER ||
  //  expr->type() == ExprType::VALUES ||
  //  expr->type() == ExprType::STAR ||
  //  expr->type() == ExprType::VECTOR_DISTANCE_EXPR){
  //   // select * from table_alias_1 t1 where id in (select t2.id from table_alias_2 t2 where t2.col2 >= t1.col1);
  //   // subquery 单独处理
  //   return RC::SUCCESS;
  // }
  if (expr->type() == ExprType::ARITHMETIC) {
    ArithmeticExpr *arith_expr = static_cast<ArithmeticExpr *>(expr);
    if (arith_expr->left() != nullptr) {
      RC rc = SelectStmt::convert_alias_to_name(arith_expr->left().get(), alias2name, field_alias2name);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to check parent relation");
        return rc;
      }
    }
    if (arith_expr->right() != nullptr) {
      RC rc = SelectStmt::convert_alias_to_name(arith_expr->right().get(), alias2name, field_alias2name);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to check parent relation");
        return rc;
      }
    }
    return RC::SUCCESS;
  }
  if (expr->type() == ExprType::UNBOUND_AGGREGATION) {
    UnboundAggregateExpr *aggre_expr = static_cast<UnboundAggregateExpr *>(expr);
    if (aggre_expr->child() == nullptr) {
      LOG_WARN("invalid aggre expr");
      return RC::INVALID_ARGUMENT;
    }
    RC rc = SelectStmt::convert_alias_to_name(aggre_expr->child().get(), alias2name, field_alias2name);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to check parent relation");
      return rc;
    }
    return rc;
  }
  
  // 对于不需要处理别名的表达式类型，直接返回成功
  if (expr->type() == ExprType::VALUE || 
      expr->type() == ExprType::STAR ||
      expr->type() == ExprType::FIELD ||
      expr->type() == ExprType::AGGREGATION) {
    return RC::SUCCESS;
  }
  
  if (expr->type() != ExprType::UNBOUND_FIELD) {
    LOG_WARN("convert_alias_to_name: invalid expr type: %d. It should be UnoundField.", expr->type());
    return RC::SUCCESS;  // 对于不支持的类型，直接返回成功而不是失败
  }
  auto ub_field_expr = static_cast<UnboundFieldExpr *>(expr);

  // 替换 field 的表的别名为真实的表名
  if (alias2name->find(ub_field_expr->table_name()) != alias2name->end()) {
    // 如果在 alias2name 中找到了，说明是别名，需要替换为真实的表名
    std::string true_table_name = (*alias2name)[ub_field_expr->table_name()];
    LOG_DEBUG("convert alias to name: %s -> %s",ub_field_expr->table_name(), true_table_name.c_str());
    ub_field_expr->set_table_alias(ub_field_expr->table_name());
    ub_field_expr->set_table_name(true_table_name);
  }

  // 替换 field 的别名为真实的字段名
  // MYSQL 中，WHERE 不能使用别名
  // if (field_alias2name != nullptr) {
  //   if (field_alias2name->find(ub_field_expr->field_name()) != field_alias2name->end()) {
  //     // 如果在 field_alias2name 中找到了，说明是别名，需要替换为真实的字段名
  //     std::string true_field_name = (*field_alias2name)[ub_field_expr->field_name()];
  //     LOG_DEBUG("convert alias(field) to name: %s -> %s",ub_field_expr->field_name(), true_field_name.c_str());
  //     ub_field_expr->set_alias(ub_field_expr->field_name());
  //     ub_field_expr->set_field_name(true_field_name);
  //   }
  // }

  return RC::SUCCESS;
}


RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }


  
  // 
  std::shared_ptr<std::unordered_map<string, string>> name2alias = std::make_shared<std::unordered_map<string, string>>();
  std::shared_ptr<std::unordered_map<string, string>> alias2name = std::make_shared<std::unordered_map<string, string>>();
  std::shared_ptr<std::vector<string>> loaded_relation_names = std::make_shared<std::vector<string>>();
  std::shared_ptr<std::unordered_map<string, string>> field_alias2name = std::make_shared<std::unordered_map<string, string>>();

  BinderContext binder_context;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;

  std::vector<std::string> tables_alias;
  
  // 处理有表别名的查询（ALIASES 不为空且包含别名）
  bool has_table_aliases = false;
  for (size_t i = 0; i < select_sql.ALIASES.size(); i++) {
    if (!select_sql.ALIASES[i].alias.empty()) {
      has_table_aliases = true;
      break;
    }
  }
  
  if (has_table_aliases) {
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

      binder_context.add_table(table);
      tables.push_back(table);
      tables_alias.push_back(select_sql.ALIASES[i].alias);
      table_map.insert({table_name, table});
      
      // 添加表别名到 BinderContext 和 table_map
      if (!select_sql.ALIASES[i].alias.empty()) {
        binder_context.add_table_alias(select_sql.ALIASES[i].alias.c_str(), table);
        table_map.insert({select_sql.ALIASES[i].alias, table});
      }

      // 检查 alias 重复
      for (size_t j = i + 1; j < select_sql.ALIASES.size(); j++) {
        if (select_sql.ALIASES[i].alias.empty() || select_sql.ALIASES[j].alias.empty()) continue;
        if (select_sql.ALIASES[i].alias == select_sql.ALIASES[j].alias) {
          LOG_WARN("duplicate alias: %s", select_sql.ALIASES[i].alias.c_str());
          return RC::INVALID_ARGUMENT;
        }
      }

      if (!select_sql.ALIASES[i].alias.empty()) {
        // 非空才存，防止重复存到空的 alias 导致 duplicate error。
        // 在alias2name中检查 alias 是否重复
        // UPDATE: 不需要子表和外表的 alias 重复检查，因为外表的 alias 可以被子表的 alias 覆盖。
        // if (alias2name->find(select_sql.relations[i].alias) != alias2name->end()) {
        //   LOG_WARN("duplicate alias found in from statement: %s", select_sql.relations[i].alias.c_str());
        //   return RC::INVALID_ARGUMENT;
        // }
        // 一切没问题之后，
        // 备用表名和别名的映射
        name2alias->insert({table_name, select_sql.ALIASES[i].alias});
        alias2name->insert({select_sql.ALIASES[i].alias, table_name});
      }
    }
  }

  // 处理传统的 relations（向后兼容）- 只在没有表别名时处理
  if (!has_table_aliases) {
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
  }
  
  // 将 expressions（要 select 的表达式）中带有别名的表名替换为真实的表名
  for (auto &expression : select_sql.expressions) {
    RC rc = convert_alias_to_name(expression.get(), alias2name, nullptr);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to convert alias to name");
      return rc;
    }

    // 如果是字段表达式并且有别名，将字段名和别名的映射存起来
    if (expression->type() == ExprType::UNBOUND_FIELD) {
      UnboundFieldExpr *ub_field_expr = static_cast<UnboundFieldExpr *>(expression.get());
      if (!ub_field_expr->alias_std_string().empty()) {
        field_alias2name->insert({ub_field_expr->alias_std_string(), ub_field_expr->field_name()});
      }
    }

    // 如果是 StarExpr，检查是否有别名，如果有报错
    if (expression->type() == ExprType::STAR) {
      StarExpr *star_expr = static_cast<StarExpr *>(expression.get());
      if (!star_expr->alias_std_string().empty()) {
        LOG_WARN("alias found in star expression");
        return RC::INVALID_ARGUMENT;
      }
    }
  }
  
   // 将 conditions 中 **所有** 带有别名的表名替换为真实的表名
   // 将 conditions 中 **所有** 使用别名 field 的表达式替换为真实的字段名
   for (auto &condition : select_sql.conditions) { 
     // 处理左表达式（新方式）
     if (condition.left_expr != nullptr) {
       RC rc = convert_alias_to_name(condition.left_expr, alias2name, field_alias2name);
       if (rc != RC::SUCCESS) {
         LOG_WARN("failed to convert alias to name in left expression");
         return rc;
       }
     } 
     // 处理右表达式（新方式）
     if (condition.right_expr != nullptr) {
       RC rc = convert_alias_to_name(condition.right_expr, alias2name, field_alias2name);
       if (rc != RC::SUCCESS) {
         LOG_WARN("failed to convert alias to name in right expression");
         return rc;
       }
     }
   }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
  // 绑定 SELECT 表达式
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
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

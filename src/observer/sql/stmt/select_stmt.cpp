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
#include "sql/expr/subquery_expr.h"
#include "sql/expr/expression.h"

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
  // 调用新的重载版本，使用默认参数
  return create(db, select_sql, stmt, nullptr, nullptr, nullptr, nullptr);
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt,
  std::shared_ptr<std::unordered_map<string, string>> name2alias,
  std::shared_ptr<std::unordered_map<string, string>> alias2name,
  std::shared_ptr<std::vector<string>> loaded_relation_names,
  std::shared_ptr<std::unordered_map<string, string>> field_alias2name
  )
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  // 别名构建与注册稍后在可见表构建完成后进行

  if (select_sql.expressions.empty()) {
    LOG_WARN("invalid argument. select attributes(exprs, technically) is empty");
    return RC::INVALID_ARGUMENT;
  }

  if (name2alias == nullptr) name2alias = std::make_shared<std::unordered_map<string, string>>();
  if (alias2name == nullptr) alias2name = std::make_shared<std::unordered_map<string, string>>();
  if (loaded_relation_names == nullptr) loaded_relation_names = std::make_shared<std::vector<string>>();
  if (field_alias2name == nullptr) field_alias2name = std::make_shared<std::unordered_map<string, string>>();

  BinderContext binder_context;
  unordered_set<string> visible_tables;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  
  // 
  // 构建本层别名映射（不继承外层，允许子查询遮蔽外层同名别名）
  unordered_map<string, string> table_alias_map;  // 别名 -> 表名映射（仅当前层）
  unordered_map<string, string> field_alias_map = field_alias2name ? *field_alias2name : unordered_map<string, string>{};  // 字段别名映射

  // 别名映射不做预处理，统一在可见表确定后构建，避免重复与误判
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
    visible_tables.insert(table_name);
    }

  // 先不基于 ALIASES 直接注册别名，稍后统一基于可见表过滤
  
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
    visible_tables.insert(table_name);
    // 处理 JOIN 条件（此处略）
  }

  // 基于本层可见表构建别名映射（别名遮蔽外层）
  for (const auto &a : select_sql.ALIASES) {
    if (a.alias.empty()) continue;
    if (!visible_tables.count(a.name)) continue;
    table_alias_map[a.alias] = a.name;
  }
  for (const auto &tr : select_sql.table_references) {
    if (tr.alias.empty()) continue;
    if (!visible_tables.count(tr.table_name)) continue;
    table_alias_map[tr.alias] = tr.table_name;
  }

  // 仅由当前层 FROM 抽取的严格可见表集合（不受任何外层影响）
  unordered_set<string> level_visible_tables;
  for (const auto &name : select_sql.relations) {
    LOG_WARN("level_visible_insert: %s", name.c_str());
    level_visible_tables.insert(name);
  }
  // // for (const auto &tr : select_sql.table_references) {
  //   level_visible_tables.insert(tr.table_name);
  // }

  // 注册别名到本层上下文
  for (const auto &kv : table_alias_map) { 
    auto it = table_map.find(kv.second);
    if (it == table_map.end()) continue;
    Table *tbl = it->second;
    table_map.emplace(kv.first, tbl);
    binder_context.add_table_alias(kv.first.c_str(), tbl);
  }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
  // 在绑定表达式之前，先解析表别名引用 & 单表默认绑定
  {
    // 调试：打印本层可见表与别名映射
    std::string tbls;
    for (auto *t : tables) { if (!tbls.empty()) tbls += ","; tbls += t->name(); }
    std::string vtbls;
    for (const auto &n : visible_tables) { if (!vtbls.empty()) vtbls += ","; vtbls += n; }
    std::string lvtbls;
    for (const auto &n : level_visible_tables) { if (!lvtbls.empty()) lvtbls += ","; lvtbls += n; }
    std::string aliases;
    for (const auto &kv : table_alias_map) { if (!aliases.empty()) aliases += ","; aliases += kv.first + string("->") + kv.second; }
    LOG_WARN("[expr prebind] tables={%s} visible={%s} level_visible={%s} aliases={%s}", tbls.c_str(), vtbls.c_str(), lvtbls.c_str(), aliases.c_str());
  }
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
     // 如果是 UnboundFieldExpr，需要解析表别名
     if (expression->type() == ExprType::UNBOUND_FIELD) {
       UnboundFieldExpr *field_expr = static_cast<UnboundFieldExpr*>(expression.get());
        LOG_WARN("[expr prebind] UNBOUND_FIELD before: table=\"%s\" field=\"%s\"", 
          field_expr->table_name() ? field_expr->table_name() : "", 
          field_expr->field_name() ? field_expr->field_name() : "");
       if (field_expr->table_name() != nullptr && strlen(field_expr->table_name()) > 0) {
          // 本层表别名映射
         auto it = table_alias_map.find(field_expr->table_name());
         if (it != table_alias_map.end()) {
           field_expr->set_table_name(it->second.c_str());
            LOG_WARN("[expr prebind] alias mapped: %s -> %s", it->first.c_str(), it->second.c_str());
          } else {
            // 没有在本层找到，直接用原表名，下一步bind会处理到相关子查询
          }
        } else if (level_visible_tables.size() == 1) {
          // 只在本层唯一表时允许默认绑定
          const std::string only_name = *level_visible_tables.begin();
          field_expr->set_table_name(only_name.c_str());
          LOG_WARN("[expr prebind] default single visible bind: %s", only_name.c_str());
        } else if (tables.size() == 1) {
          field_expr->set_table_name(tables[0]->name());
          LOG_WARN("[expr prebind] default single table bind: %s", tables[0]->name());
        } else if (table_alias_map.size() == 1) {
          const auto &only = *table_alias_map.begin();
          field_expr->set_table_name(only.second.c_str());
          LOG_WARN("[expr prebind] default single alias bind: %s -> %s", only.first.c_str(), only.second.c_str());
        } else {
          // 多于1表/别名，禁止绑定裸字段
          std::string tbls; for (auto *t : tables) { if (!tbls.empty()) tbls += ","; tbls += t->name(); }
          std::string aliases; for (const auto &kv : table_alias_map) { if (!aliases.empty()) aliases += ","; aliases += kv.first + string("->") + kv.second; }
          LOG_WARN("[expr prebind] ambiguous UNBOUND_FIELD (no default bind): field='%s', multi-table/alias query! tables={%s} aliases={%s}", field_expr->field_name(), tbls.c_str(), aliases.c_str());
          return RC::SCHEMA_FIELD_NOT_EXIST;
        }
        LOG_WARN("[expr prebind] UNBOUND_FIELD after: table=\"%s\" field=\"%s\"", 
          field_expr->table_name() ? field_expr->table_name() : "", 
          field_expr->field_name() ? field_expr->field_name() : "");
     }
    
    RC rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s expr_type=%d", strrc(rc), (int)expression->type());
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

  // 转换WHERE条件中的表别名
  for (auto &condition : select_sql.conditions) {
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
    // 默认绑定未指定表的列（仅一个表时）
    // 只允许本层唯一表时允许default bind，否则都需要带表名
    // if (level_visible_tables.size() == 1) {
    //   const std::string only_name = *level_visible_tables.begin();
    //   if (condition.left_is_attr && condition.left_attr.relation_name.empty()) {
    //     condition.left_attr.relation_name = only_name;
    //   }
    //   if (condition.right_is_attr && condition.right_attr.relation_name.empty()) {
    //     condition.right_attr.relation_name = only_name;
    //   }
    // } else if (tables.size() == 1) {
    //   if (condition.left_is_attr && condition.left_attr.relation_name.empty()) {
    //     condition.left_attr.relation_name = tables[0]->name();
    //   }
    //   if (condition.right_is_attr && condition.right_attr.relation_name.empty()) {
    //     condition.right_attr.relation_name = tables[0]->name();
    //   }
    // } else if (table_alias_map.size() == 1) {
    //   const auto &only = *table_alias_map.begin();
    //   if (condition.left_is_attr && condition.left_attr.relation_name.empty()) {
    //     condition.left_attr.relation_name = only.second;
    //   }
    //   if (condition.right_is_attr && condition.right_attr.relation_name.empty()) {
    //     condition.right_attr.relation_name = only.second;
    //   }
    // } else {
    //   if ((condition.left_is_attr && condition.left_attr.relation_name.empty()) ||
    //       (condition.right_is_attr && condition.right_attr.relation_name.empty())) {
    //     std::string tbls; for (auto *t : tables) { if (!tbls.empty()) tbls += ","; tbls += t->name(); }
    //     std::string aliases; for (const auto &kv : table_alias_map) { if (!aliases.empty()) aliases += ","; aliases += kv.first + string("->") + kv.second; }
    //     LOG_WARN("[where/join bind] ambiguous attr: left='%s', right='%s', multi-table/alias query! Must specify table/alias. tables={%s} aliases={%s}", condition.left_attr.attribute_name.c_str(), condition.right_attr.attribute_name.c_str(), tbls.c_str(), aliases.c_str());
    //     return RC::SCHEMA_FIELD_NOT_EXIST;
    //   }
    // }
    // 如有外层相关列需求，需在表达式阶段处理，这里不创建相关列表达式
  }

  for (auto &condition : select_sql.conditions) {
    // exists/not exists 可能会使得 left_expr 为空
    if (condition.left_expr != nullptr && condition.left_expr->type() == ExprType::SUB_QUERY) {
      SubqueryExpr *subquery_expr = static_cast<SubqueryExpr *>(condition.left_expr);
      Stmt         *stmt          = nullptr;
      RC            rc            = SelectStmt::create(
        db, 
        subquery_expr->sub_query_sn()->selection, 
        stmt, 
        name2alias, 
        alias2name, 
        loaded_relation_names,
        field_alias2name
      );
      if (rc != RC::SUCCESS) {
        LOG_WARN("cannot construct subquery stmt");
        return rc;
      }
      // 检查子查询的合法性：子查询的查询的属性只能有一个
      RC rc_ = check_sub_select_legal(db, subquery_expr->sub_query_sn());
      if (rc_ != RC::SUCCESS) {
        return rc_;
      }
      subquery_expr->set_stmt(unique_ptr<SelectStmt>(static_cast<SelectStmt *>(stmt)));
    }
    if (condition.right_expr != nullptr && condition.right_expr->type() == ExprType::SUB_QUERY) {
      SubqueryExpr *subquery_expr = static_cast<SubqueryExpr *>(condition.right_expr);
      // subquery on right expression
      Stmt         *stmt          = nullptr;
      RC            rc            = SelectStmt::create(
        db,
        subquery_expr->sub_query_sn()->selection, 
        stmt, 
        name2alias, 
        alias2name, 
        loaded_relation_names,
        field_alias2name
      );
      if (rc != RC::SUCCESS) {
        return rc;
      }
      // 检查子查询的合法性：子查询的查询的属性只能有一个
      RC rc_ = check_sub_select_legal(db, subquery_expr->sub_query_sn());
      if (rc_ != RC::SUCCESS) {
        return rc_;
      }
      subquery_expr->set_stmt(unique_ptr<SelectStmt>(static_cast<SelectStmt *>(stmt)));
    }
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
          
          all_join_conditions.push_back(std::move(condition));
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

RC SelectStmt::convert_alias_to_name(Expression *expr, 
std::shared_ptr<std::unordered_map<string, string>> alias2name,
std::shared_ptr<std::unordered_map<string, string>> field_alias2name) {
   if (expr->type() == ExprType::VALUE || 
   expr->type() == ExprType::SUB_QUERY || 
   expr->type() == ExprType::STAR){
    // select * from table_alias_1 t1 where id in (select t2.id from table_alias_2 t2 where t2.col2 >= t1.col1);
    // subquery 单独处理
    return RC::SUCCESS;
  }
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
  
  if (expr->type() != ExprType::UNBOUND_FIELD) {
    LOG_WARN("convert_alias_to_name: invalid expr type: %d. It should be UnoundField.", expr->type());
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

  return RC::SUCCESS;
}

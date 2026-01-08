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
<<<<<<< HEAD
=======
  
  // 首先将 loaded_relation_names 中的表名添加到 table_map 中
  // 同时将外层查询的别名也添加到 table_map，以便子查询可以通过别名访问外层表
  for (auto &rel_name : *loaded_relation_names) {
    if (table_map.find(rel_name) != table_map.end()) {
      continue;
    }
    Table *table = db->find_table(rel_name.c_str());
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), rel_name.c_str());
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
    table_map.insert({rel_name, table});
    binder_context.add_table(table);
    
    // 查找该表对应的别名，并将别名也添加到 table_map
    if (alias2name) {
      for (const auto &pair : *alias2name) {
        if (pair.second == rel_name) {
          table_map.insert({pair.first, table});
          binder_context.add_table_alias(pair.first.c_str(), table);
        }
      }
    }
  }
  
  // 
  // 构建本层别名映射（不继承外层，允许子查询遮蔽外层同名别名）
  unordered_map<string, string> table_alias_map;  // 别名 -> 表名映射（仅当前层）
  unordered_map<string, string> field_alias_map = field_alias2name ? *field_alias2name : unordered_map<string, string>{};  // 字段别名映射

  // 别名映射不做预处理，统一在可见表确定后构建，避免重复与误判
// 处理字段别名
  for (size_t i = 0; i < select_sql.expressions.size(); i++) {
      Expression *expr = select_sql.expressions[i].get();
      // 检查独立的 StarExpr（*）是否有别名，如果有则报错
      // 注意：不检查聚合函数内的 *（如 count(*)），因为那些会在绑定阶段被替换为 ValueExpr
      if (expr->type() == ExprType::STAR) {
        StarExpr *star_expr = static_cast<StarExpr *>(expr);
        if (star_expr->has_alias()) {
          LOG_WARN("Cannot use alias with '*' (star expression). alias: %s", star_expr->alias());
          return RC::INVALID_ARGUMENT;
        }
      }
      if (expr->has_alias()) {
          field_alias_map[expr->alias()] = expr->name();
      }
  }

  // 处理传统的 relations（向后兼容）
  // 注意：这应该只包含当前查询层的表，不应该包含外层的表
>>>>>>> d928428f91fe5f4dcbe8f4a5a5f8041146971e2f
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
<<<<<<< HEAD
=======
    visible_tables.insert(table_name);
    loaded_relation_names->push_back(table_name);
  }

  // 先不基于 ALIASES 直接注册别名，稍后统一基于可见表过滤
  
  // 调试：打印 table_references 和 relations 信息
  std::string trefs_debug;
  for (const auto &tr : select_sql.table_references) {
    if (!trefs_debug.empty()) trefs_debug += ",";
    trefs_debug += tr.table_name + (tr.alias.empty() ? "" : " AS " + tr.alias);
  }
  std::string rels_debug;
  for (const auto &rel : select_sql.relations) {
    if (!rels_debug.empty()) rels_debug += ",";
    rels_debug += rel;
  }
  LOG_WARN("[table collect] table_references={%s}, relations={%s}", trefs_debug.c_str(), rels_debug.c_str());
  
  // 处理新的 table_references（支持 JOIN）
  // 注意：这应该只包含当前查询层的表，不应该包含外层的表
  // 如果 table_references 被错误填充了外层的表，这里需要过滤
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
    loaded_relation_names->push_back(table_name);
  }
  
  // 确保 visible_tables 只包含当前层的表（用于别名重复检查）
  // 但保留外层表在 table_map 中（用于相关子查询的字段绑定）
  // 如果解析器错误地把外层的表也放在了 table_references 中，我们需要基于 relations 来过滤 visible_tables
  // 因为 relations 通常只包含当前层的表（从 FROM 子句解析而来）
  // 注意：即使过滤了 visible_tables，外层表仍然保留在 table_map 中，这样相关子查询可以访问外层表
  unordered_set<string> filtered_visible_tables;
  if (!select_sql.relations.empty()) {
    // 如果 relations 不为空，说明这是传统语法，visible_tables 应该只包含 relations 中的表
    for (const auto &rel : select_sql.relations) {
      if (visible_tables.count(rel)) {
        filtered_visible_tables.insert(rel);
      }
    }
    // 只有当 filtered_visible_tables 和 visible_tables 不一致时才替换
    // 这样可以避免误过滤，但能修复子查询的 visible_tables 被污染的问题
    if (!filtered_visible_tables.empty() && filtered_visible_tables.size() < visible_tables.size()) {
      std::string old_vtbls;
      for (const auto &t : visible_tables) { if (!old_vtbls.empty()) old_vtbls += ","; old_vtbls += t; }
      std::string new_vtbls;
      for (const auto &t : filtered_visible_tables) { if (!new_vtbls.empty()) new_vtbls += ","; new_vtbls += t; }
      LOG_WARN("[table filter] filter visible_tables from {%s} to {%s} (for alias check only, outer tables still in table_map)", old_vtbls.c_str(), new_vtbls.c_str());
      // 重要：只更新 visible_tables（用于别名检查），但保留 table_map 中的外层表（用于相关子查询）
      // visible_tables 现在只用于别名重复检查和默认别名绑定
      visible_tables = filtered_visible_tables;
    }
  }

  // 基于本层可见表构建别名映射（别名遮蔽外层）
  // 检查重复的表别名：只在 ALIASES 内部检查重复（仅本层可见表的别名）
  // 关键：只处理本层 visible_tables 中的表的别名，忽略不在本层的表（可能是外层的）
  unordered_map<string, string> alias_to_table;  // 别名 -> 表名，用于检查重复
  
  // 调试：打印 ALIASES 和 visible_tables 信息
  std::string aliases_debug;
  for (const auto &a : select_sql.ALIASES) {
    if (!aliases_debug.empty()) aliases_debug += ",";
    aliases_debug += a.alias + ":" + a.name;
  }
  std::string vtbls_debug;
  for (const auto &n : visible_tables) { if (!vtbls_debug.empty()) vtbls_debug += ","; vtbls_debug += n; }
  LOG_WARN("[alias check] ALIASES={%s}, visible_tables={%s}", aliases_debug.c_str(), vtbls_debug.c_str());
  
  for (const auto &a : select_sql.ALIASES) {
    if (a.alias.empty()) continue;
    // 只处理本层可见表的别名，跳过外层表的别名（用于重复检查）
    // 这是关键：子查询的 ALIASES 可能包含外层表的别名，但 visible_tables 只包含本层表
    if (!visible_tables.count(a.name)) {
      // 这个别名指向的表不在本层可见表中，跳过（可能是外层的）
      // 但是，如果是相关子查询，外层表的别名仍然需要添加到 table_alias_map（但不用于重复检查）
      // 例如：子查询的 ALIASES 中可能有 {alias: "s1", name: "ssq_1"}（外层）
      // 虽然不在 visible_tables 中，但如果表在 table_map 中（说明是外层表），仍然添加别名映射
      if (table_map.count(a.name)) {
        // 外层表的别名，添加到 table_alias_map 但不检查重复（允许与外层同名）
        LOG_WARN("[alias check] add outer table alias %s:%s (for correlated subquery)", a.alias.c_str(), a.name.c_str());
        table_alias_map[a.alias] = a.name;
      } else {
        LOG_WARN("[alias check] skip alias %s:%s (not in visible_tables and not in table_map)", a.alias.c_str(), a.name.c_str());
      }
      continue;  // 跳过重复检查
    }
    // 检查本层可见表的别名是否有重复
    auto it = alias_to_table.find(a.alias);
    if (it != alias_to_table.end()) {
      // 如果别名已存在且指向不同的表，这是真正的重复（同一层中同一别名指向不同表）
      if (it->second != a.name) {
        // 输出详细的调试信息
        std::string vtbls;
        for (const auto &n : visible_tables) { if (!vtbls.empty()) vtbls += ","; vtbls += n; }
        LOG_WARN("Duplicate table alias in same query level: %s -> %s vs %s (visible_tables={%s})", 
                 a.alias.c_str(), it->second.c_str(), a.name.c_str(), vtbls.c_str());
        return RC::INVALID_ARGUMENT;
      }
      // 如果别名已存在且指向同一个表，可能是 ALIASES 数组中的重复项（同步导致），跳过
      LOG_WARN("[alias check] skip duplicate alias entry %s:%s (same table)", a.alias.c_str(), a.name.c_str());
      continue;
    }
    // 添加到本层别名映射
    LOG_WARN("[alias check] add alias %s:%s", a.alias.c_str(), a.name.c_str());
    alias_to_table[a.alias] = a.name;
    table_alias_map[a.alias] = a.name;
  }
  // table_references 的别名添加到映射（不检查与 ALIASES 的冲突，因为它们可能是同步的）
  for (const auto &tr : select_sql.table_references) {
    if (tr.alias.empty()) continue;
    if (!visible_tables.count(tr.table_name)) continue;
    // 如果别名已存在（来自 ALIASES），则跳过，不报错
    // 如果别名不存在，则添加到映射
    if (table_alias_map.find(tr.alias) == table_alias_map.end()) {
      table_alias_map[tr.alias] = tr.table_name;
    }
  }

  // 仅由当前层 FROM 抽取的严格可见表集合（不受任何外层影响）
  // 这是用于字段默认绑定的严格集合，只包含当前层的表
  // 关键：只使用 relations（传统语法）或 table_references（新语法）中的表，避免包含外层表
  unordered_set<string> level_visible_tables;
  // 优先使用 relations（更可靠，通常只包含当前层的表）
  for (const auto &name : select_sql.relations) {
    LOG_WARN("level_visible_insert: %s", name.c_str());
    level_visible_tables.insert(name);
  }
  // 如果 relations 为空，才使用 table_references（但需要过滤，避免包含外层表）
  if (level_visible_tables.empty()) {
    // 使用过滤后的 visible_tables（只包含当前层的表）
    level_visible_tables = visible_tables;
  } else {
    // 如果 relations 不为空，只使用 relations 中的表（更安全，避免外层表污染）
    // 不添加 table_references 中的表，因为可能包含外层表
  }

  // 注册别名到本层上下文，并更新 name2alias 和 alias2name 以便传递给子查询
  for (const auto &kv : table_alias_map) { 
    auto it = table_map.find(kv.second);
    if (it == table_map.end()) continue;
    Table *tbl = it->second;
    table_map.emplace(kv.first, tbl);
    binder_context.add_table_alias(kv.first.c_str(), tbl);
    // 更新 name2alias 和 alias2name，以便子查询可以访问外层查询的别名
    if (name2alias) {
      name2alias->insert({kv.second, kv.first});
    }
    if (alias2name) {
      alias2name->insert({kv.first, kv.second});
    }
>>>>>>> d928428f91fe5f4dcbe8f4a5a5f8041146971e2f
  }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
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
  RC rc = FilterStmt::create(db, default_table, &table_map, select_sql.conditions, filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  // create filter statement in `having` statement
  FilterStmt *filter_stmt_having = nullptr;
  if (!select_sql.havings.empty()) {
    RC rc = FilterStmt::create(db, default_table, &table_map, select_sql.havings, filter_stmt_having);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct filter stmt");
      return rc;
    }
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();

  select_stmt->tables_.swap(tables);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->filter_stmt_having_ = filter_stmt_having;
  stmt                      = select_stmt;
  return RC::SUCCESS;
}
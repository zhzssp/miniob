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
// Created by Wangyunlai on 2023/6/13.
//

#pragma once

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "sql/stmt/stmt.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/expr/expression.h"
class Db;

/**
 * @brief 表示创建表的语句
 * @ingroup Statement
 * @details 虽然解析成了stmt，但是与原始的SQL解析后的数据也差不多
 */

class UpdateStmt : public Stmt {
public:
  UpdateStmt(Table *table, const char *attribute_name, unique_ptr<Expression> value_expr, FilterStmt *filter_stmt)
    : table_(table), attribute_name_(attribute_name), value_expr_(std::move(value_expr)), filter_stmt_(filter_stmt)
{}
  ~UpdateStmt()
  {
    if (nullptr != filter_stmt_) {
      delete filter_stmt_;
      filter_stmt_ = nullptr;
    }
  }
  StmtType type() const override { return StmtType::UPDATE; }
  Table *table() const { return table_; }
  const char *attribute_name() const { return attribute_name_; }
  Expression *value_expr() const { return value_expr_.get(); }
  FilterStmt *filter_stmt() const { return filter_stmt_; }
  
  static RC create(Db *db, const UpdateSqlNode &update_sql, Stmt *&stmt);
  
private:
  Table *table_;
  const char *attribute_name_;  // 要更新的字段名
  unique_ptr<Expression> value_expr_;  // 新值表达式，支持子查询
  FilterStmt *filter_stmt_;    // WHERE条件
};

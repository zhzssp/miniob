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
// Created by Wangyunlai on 2023/4/25.
//

#pragma once

#include "sql/stmt/stmt.h"

struct CreateIndexSqlNode;
class Table;
class FieldMeta;

/**
 * @brief 创建索引的语句
 * @ingroup Statement
 */
class CreateIndexStmt : public Stmt
{
public:
  CreateIndexStmt(Table *table, const FieldMeta *field_meta, const string &index_name, bool is_unique)
      : table_(table), field_meta_(field_meta), index_name_(index_name), is_unique_(is_unique)
  {
    if (field_meta != nullptr) {
      fields_meta_.push_back(field_meta);
    }
  }

  CreateIndexStmt(Table *table, const vector<const FieldMeta *> &fields_meta, const string &index_name, bool is_unique)
      : table_(table), fields_meta_(fields_meta), index_name_(index_name), is_unique_(is_unique)
  {
    if (!fields_meta.empty()) {
      field_meta_ = fields_meta[0];  // 向后兼容
    }
  }

  virtual ~CreateIndexStmt() = default;

  StmtType type() const override { return StmtType::CREATE_INDEX; }

  Table           *table() const { return table_; }
  const FieldMeta *field_meta() const { return field_meta_; }  // 向后兼容：返回第一个字段
  const vector<const FieldMeta *> &fields_meta() const { return fields_meta_; }
  const string    &index_name() const { return index_name_; }
  bool             is_unique() const { return is_unique_; }

public:
  static RC create(Db *db, const CreateIndexSqlNode &create_index, Stmt *&stmt);

private:
  Table           *table_      = nullptr;
  const FieldMeta *field_meta_ = nullptr;  // 向后兼容：第一个字段
  vector<const FieldMeta *> fields_meta_;  // 支持复合索引
  string           index_name_;
  bool             is_unique_   = false;
};

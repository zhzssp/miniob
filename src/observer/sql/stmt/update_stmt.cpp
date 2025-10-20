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

  // 检查字段是否存在
  const TableMeta &table_meta = table->table_meta();
  const FieldMeta *field_meta = table_meta.field(update_sql.attribute_name.c_str());
  if (nullptr == field_meta) {
    LOG_WARN("no such field. field=%s.%s.%s", db->name(), table->name(), update_sql.attribute_name.c_str());
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  // 检查值类型是否兼容，如果不兼容则尝试转换
  const AttrType field_type = field_meta->type();
  const AttrType value_type = update_sql.value.attr_type();
  
  Value final_value = update_sql.value;
  if (field_type != value_type) {
    // 尝试类型转换
    RC cast_rc = Value::cast_to(update_sql.value, field_type, final_value);
    if (cast_rc != RC::SUCCESS) {
      LOG_WARN("type mismatch and cannot cast. field=%s.%s.%s, field_type=%d, value_type=%d",
               db->name(), table->name(), update_sql.attribute_name.c_str(), field_type, value_type);
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
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

  stmt = new UpdateStmt(table, update_sql.attribute_name.c_str(), new Value(final_value), filter_stmt);

  return RC::SUCCESS;
}
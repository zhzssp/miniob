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
// Created by Wangyunlai.wyl on 2021/5/18.
//

#include "storage/index/index_meta.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/field/field_meta.h"
#include "storage/table/table_meta.h"
#include "json/json.h"

const static Json::StaticString FIELD_NAME("name");
const static Json::StaticString FIELD_FIELD_NAME("field_name");
const static Json::StaticString FIELD_FIELD_NAMES("field_names");
const static Json::StaticString FIELD_IS_UNIQUE("is_unique");

RC IndexMeta::init(const char *name, const FieldMeta &field, bool is_unique)
{
  if (common::is_blank(name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }

  name_  = name;
  field_ = field.name();
  fields_.clear();
  fields_.push_back(field.name());
  is_unique_ = is_unique;
  return RC::SUCCESS;
}

RC IndexMeta::init(const char *name, const vector<const FieldMeta *> &fields, bool is_unique)
{
  if (common::is_blank(name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }
  if (fields.empty()) {
    LOG_ERROR("Failed to init index, no fields provided.");
    return RC::INVALID_ARGUMENT;
  }

  name_  = name;
  fields_.clear();
  for (const FieldMeta *field : fields) {
    if (field == nullptr) {
      LOG_ERROR("Failed to init index, null field provided.");
      return RC::INVALID_ARGUMENT;
    }
    fields_.push_back(field->name());
  }
  field_ = fields_[0];  // 向后兼容：第一个字段名
  is_unique_ = is_unique;
  return RC::SUCCESS;
}

void IndexMeta::to_json(Json::Value &json_value) const
{
  json_value[FIELD_NAME]       = name_;
  json_value[FIELD_IS_UNIQUE]  = is_unique_;
  
  // 向后兼容：如果只有一个字段，使用 field_name
  if (fields_.size() == 1) {
    json_value[FIELD_FIELD_NAME] = fields_[0];
  } else {
    // 多个字段使用 field_names 数组
    Json::Value fields_array(Json::arrayValue);
    for (const string &field_name : fields_) {
      fields_array.append(field_name);
    }
    json_value[FIELD_FIELD_NAMES] = fields_array;
    // 仍然保留 field_name 用于向后兼容
    json_value[FIELD_FIELD_NAME] = fields_[0];
  }
}

RC IndexMeta::from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index)
{
  const Json::Value &name_value  = json_value[FIELD_NAME];
  if (!name_value.isString()) {
    LOG_ERROR("Index name is not a string. json value=%s", name_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  bool is_unique = false;
  const Json::Value &is_unique_value = json_value[FIELD_IS_UNIQUE];
  if (is_unique_value.isBool()) {
    is_unique = is_unique_value.asBool();
  }

  // 优先检查 field_names（复合索引）
  const Json::Value &fields_value = json_value[FIELD_FIELD_NAMES];
  if (fields_value.isArray() && fields_value.size() > 0) {
    vector<const FieldMeta *> fields;
    for (const Json::Value &field_value : fields_value) {
      if (!field_value.isString()) {
        LOG_ERROR("Field name in index [%s] is not a string. json value=%s",
            name_value.asCString(), field_value.toStyledString().c_str());
        return RC::INTERNAL;
      }
      const FieldMeta *field = table.field(field_value.asCString());
      if (nullptr == field) {
        LOG_ERROR("Deserialize index [%s]: no such field: %s", name_value.asCString(), field_value.asCString());
        return RC::SCHEMA_FIELD_MISSING;
      }
      fields.push_back(field);
    }
    return index.init(name_value.asCString(), fields, is_unique);
  }

  // 向后兼容：单个字段使用 field_name
  const Json::Value &field_value = json_value[FIELD_FIELD_NAME];
  if (!field_value.isString()) {
    LOG_ERROR("Field name of index [%s] is not a string. json value=%s",
        name_value.asCString(), field_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  const FieldMeta *field = table.field(field_value.asCString());
  if (nullptr == field) {
    LOG_ERROR("Deserialize index [%s]: no such field: %s", name_value.asCString(), field_value.asCString());
    return RC::SCHEMA_FIELD_MISSING;
  }

  return index.init(name_value.asCString(), *field, is_unique);
}

const char *IndexMeta::name() const { return name_.c_str(); }

const char *IndexMeta::field() const { return field_.c_str(); }

void IndexMeta::desc(ostream &os) const { os << "index name=" << name_ << ", field=" << field_; }
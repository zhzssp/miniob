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
const static Json::StaticString FIELD_FIELD_NAMES("field_names");
const static Json::StaticString FIELD_TYPES("field_types");
const static Json::StaticString FIELD_LENGTHS("field_lengths");

RC IndexMeta::init(const char *index_name, const vector<const FieldMeta *> &field_metas)
{
  if (common::is_blank(index_name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }
  if (field_metas.empty()) {
    LOG_ERROR("Failed to init index, fields is empty.");
    return RC::INVALID_ARGUMENT;
  }

  fields_.clear();
  field_types_.clear();
  field_lengths_.clear();

  fields_.reserve(field_metas.size());
  field_types_.reserve(field_metas.size());
  field_lengths_.reserve(field_metas.size());

  name_ = index_name;
  for (const FieldMeta *field_meta : field_metas) {
    if(field_meta == nullptr) {
      LOG_ERROR("When initialize IndexMeta, get null field_meta");
      return RC::INVALID_ARGUMENT;
    }
    fields_.push_back(string(field_meta->name()));
    field_types_.push_back(field_meta->type());
    field_lengths_.push_back(field_meta->len());
  }
  return RC::SUCCESS;
}

void IndexMeta::to_json(Json::Value &json_value) const
{
  json_value[FIELD_NAME] = name_;
  Json::Value arr_names(Json::arrayValue);
  Json::Value arr_types(Json::arrayValue);
  Json::Value arr_lengths(Json::arrayValue);
  for (size_t i = 0; i < fields_.size(); i++) {
    arr_names.append(fields_[i]);
    arr_types.append(static_cast<int>(field_types_[i]));
    arr_lengths.append(field_lengths_[i]);
  }
  json_value[FIELD_FIELD_NAMES] = arr_names;
  json_value[FIELD_TYPES]       = arr_types;
  json_value[FIELD_LENGTHS]     = arr_lengths;
}

RC IndexMeta::from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index)
{
  const Json::Value &name_value = json_value[FIELD_NAME];
  if (!name_value.isString()) {
    LOG_ERROR("Index name is not a string. json value=%s", name_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  vector<const FieldMeta *>  fields;
  const Json::Value &names   = json_value[FIELD_FIELD_NAMES];
  const Json::Value &types   = json_value[FIELD_TYPES];
  const Json::Value &lengths = json_value[FIELD_LENGTHS];
  if (names.isArray() && types.isArray() && lengths.isArray() && names.size() == types.size() &&
      names.size() == lengths.size()) {
    for (Json::ArrayIndex i = 0; i < names.size(); i++) {
      const char      *fname = names[i].asCString();
      const FieldMeta *fm    = table.field(fname);
      if (fm == nullptr) {
        LOG_ERROR("Deserialize index [%s]: no such field: %s", name_value.asCString(), fname);
        return RC::SCHEMA_FIELD_MISSING;
      }
      fields.push_back(fm);
    }
    return index.init(name_value.asCString(), fields);
  }

  // fallback: 兼容旧格式
  const Json::Value &field_value = json_value["field_name"];  // legacy key
  if (field_value.isString()) {
    const FieldMeta *field = table.field(field_value.asCString());
    if (nullptr == field) {
      LOG_ERROR("Deserialize index [%s]: no such field: %s", name_value.asCString(), field_value.asCString());
      return RC::SCHEMA_FIELD_MISSING;
    }
    vector<const FieldMeta *> one{field};
    return index.init(name_value.asCString(), one);
  }

  LOG_ERROR("Invalid index meta json: %s", json_value.toStyledString().c_str());
  return RC::INTERNAL;
}

const char *IndexMeta::name() const { return name_.c_str(); }

void IndexMeta::desc(ostream &os) const
{
  os << "index name=" << name_ << ", fields=[";
  for (size_t i = 0; i < fields_.size(); i++) {
    if (i > 0)
      os << ",";
    os << fields_[i];
  }
  os << "]";
}

RC IndexMeta::build_composite_key(const char *record, char *composite_key) const
{
  if (fields_.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  int offset = 0;
  for (size_t i = 0; i < fields_.size(); i++) {
    // 这里需要获取字段在记录中的偏移量
    // 需要传入FieldMeta信息或TableMeta
    // 暂时用field_lengths_作为示例
    memcpy(composite_key + offset, record + offset, field_lengths_[i]);
    offset += field_lengths_[i];
  }

  return RC::SUCCESS;
}

RC IndexMeta::extract_field_from_composite_key(const char *composite_key, int field_index, char *field_value) const
{
  if (field_index < 0 || field_index >= static_cast<int>(fields_.size())) {
    return RC::INVALID_ARGUMENT;
  }

  int offset = field_offset(field_index);
  int length = field_lengths_[field_index];
  memcpy(field_value, composite_key + offset, length);

  return RC::SUCCESS;
}

int IndexMeta::field_offset(int field_index) const
{
  int offset = 0;
  for (int i = 0; i < field_index; i++) {
    offset += field_lengths_[i];
  }
  return offset;
}

vector<string> IndexMeta::fields() const
{
  return fields_;
}

const char *IndexMeta::field(int index) const
{
  if (index < 0 || index >= static_cast<int>(fields_.size())) {
    return nullptr;
  }
  return fields_[index].c_str();
}

AttrType IndexMeta::field_type(int index) const
{
  if (index < 0 || index >= static_cast<int>(field_types_.size())) {
    return AttrType::UNDEFINED;
  }
  return field_types_[index];
}

int IndexMeta::field_length(int index) const
{
  if (index < 0 || index >= static_cast<int>(field_lengths_.size())) {
    return -1;
  }
  return field_lengths_[index];
}
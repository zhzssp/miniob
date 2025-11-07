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

RC IndexMeta::init(const char *name, vector<const FieldMeta *> field_metas)
{
  if (common::is_blank(name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }

  name_  = name;

  fields_.reserve(field_metas.size());
  for(const FieldMeta *field_meta: field_metas) {
    fields_.emplace_back(field_meta->name());
  }
  return RC::SUCCESS;
}

RC IndexMeta::init(const char *name, vector<string> fields)
{
  if (common::is_blank(name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }

  name_  = name;

  fields_.reserve(fields.size());
  for(string field: fields) {
    fields_.emplace_back(field);
  }
  return RC::SUCCESS;
}

void IndexMeta::to_json(Json::Value &json_value) const
{
  json_value[FIELD_NAME]       = name_;
  // 转化为 JSON 数组
  Json::Value fields_json(Json::arrayValue);
  for (const auto &f : fields_) {
    fields_json.append(f);
  }
  json_value[FIELD_FIELD_NAME] = fields_json;
}

RC IndexMeta::from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index)
{
  const Json::Value &name_value  = json_value[FIELD_NAME];
  const Json::Value &fields_value = json_value[FIELD_FIELD_NAME];

  // size可能有问题？
  vector<string> fields;
  fields.clear();
  if (fields_value.isArray()) {
    for (const auto &val : fields_value) {
      if (!val.isString()) {
        LOG_ERROR("Field name of index [%s] is not a string. json value=%s",
        name_value.asCString(), val.toStyledString().c_str());
        return RC::INTERNAL;
      }
      fields.push_back(val.asString());
    }
  } else {
    LOG_ERROR("fields_value is not array");
    return RC::INTERNAL;
  }

  if (!name_value.isString()) {
    LOG_ERROR("Index name is not a string. json value=%s", name_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  return index.init(name_value.asCString(), fields);
}

const char *IndexMeta::name() const { return name_.c_str(); }

const char *IndexMeta::field(int index) const { return fields_[index].c_str(); }

void IndexMeta::desc(std::ostream &os) const 
{
  os << "index name=" << name_ << ", fields=[";

  for (size_t i = 0; i < fields_.size(); i++) {
    os << fields_[i];
    if (i + 1 < fields_.size()) {
      os << ", ";
    }
  }

  os << "]";
}

int IndexMeta::field_num() const { return fields_.size(); }

IndexMeta &IndexMeta::operator=(const IndexMeta &other)
{
  name_   = other.name_;    
  fields_ = other.fields_;  

  return *this;
}
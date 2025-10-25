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
// Created by Wangyunlai on 2021/5/12.
//

#pragma once

#include "common/sys/rc.h"
#include "common/lang/string.h"
#include "common/type/attr_type.h"

class TableMeta;
class FieldMeta;

namespace Json {
class Value;
}  // namespace Json

/**
 * @brief 描述一个索引
 * @ingroup Index
 * @details 一个索引包含了表的哪些字段，索引的名称等。
 * 如果以后实现了多种类型的索引，还需要记录索引的类型，对应类型的一些元数据等
 */
class IndexMeta
{
public:
  IndexMeta() = default;

  RC init(const char *name, const vector<const FieldMeta *> &fields);

public:
  const char *name() const;

  // 获取字段信息
  int         field_count() const { return fields_.size(); }
  const char *field(int index) const;
  AttrType    field_type(int index) const;
  int         field_length(int index) const;
  vector<string> fields() const;

  // 复合键操作
  /**
   * @brief 构建复合键
   * @param record 记录数据
   * @param composite_key 输出的复合键缓冲区
   * @return RC
   */
  RC build_composite_key(const char *record, char *composite_key) const;

  /**
   * @brief 从复合键中提取指定字段
   * @param composite_key 复合键数据
   * @param field_index 字段索引
   * @param field_value 输出的字段值缓冲区
   * @return RC
   */
  RC extract_field_from_composite_key(const char *composite_key, int field_index, char *field_value) const;

  /**
   * @brief 计算字段在复合键中的偏移量
   * @param field_index 字段索引
   * @return 偏移量
   */
  int field_offset(int field_index) const;

  // 索引降序？
  void desc(ostream &os) const;

public:
  void      to_json(Json::Value &json_value) const;
  static RC from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index);

protected:
  string name_;  // index's name
  // 只有一个字段 --> 可扩展到两个
  vector<string>   fields_;  // fields' name
  vector<AttrType> field_types_;
  vector<int32_t>  field_lengths_;
};

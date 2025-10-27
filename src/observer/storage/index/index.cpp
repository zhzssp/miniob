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
// Created by wangyunlai.wyl on 2021/5/19.
//

#include "storage/index/index.h"

RC Index::init(const IndexMeta &index_meta, const vector<FieldMeta> &field_metas)
{
  if (field_metas.empty()) {
    LOG_WARN("Failed to init index %s due to empty field metas", index_meta.name());
    return RC::INVALID_ARGUMENT;
  }

  // field_metas的生命周期可能产生问题
  int count = 1;
  for (const FieldMeta &field_meta : field_metas) {
    if (field_meta.len() <= 0) {
      LOG_WARN("Found null field meta when init index %s", index_meta.name());
      return RC::INVALID_ARGUMENT;
    }
    LOG_INFO("GET field %d when init Index, length = %d", count, field_meta.len());
    count++;
  }

  index_meta_  = index_meta;

  field_metas_.clear();
  field_metas_.reserve(field_metas.size());
  field_metas_ = field_metas;
  return RC::SUCCESS;
}

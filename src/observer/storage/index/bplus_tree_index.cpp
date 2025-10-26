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

#include "storage/index/bplus_tree_index.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/db/db.h"
#include <exception>

BplusTreeIndex::~BplusTreeIndex() noexcept { close(); }

RC BplusTreeIndex::create(
    Table *table, const char *file_name, const IndexMeta &index_meta, const vector<const FieldMeta *> &field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }

  // 同步进行BplusTreeIndex内部属性的初始化
  Index::init(index_meta, field_metas);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();

  // 严格控制大小，避免错误读取多余的内存导致程序崩溃
  // 此处生命周期已经不受field_metas的控制
  vector<AttrType> attr_type;
  vector<int32_t> attr_length;
  attr_type.reserve(field_metas.size());
  attr_length.reserve(field_metas.size());

  for (const FieldMeta *field_meta : field_metas) {
    attr_type.push_back(field_meta->type());
    attr_length.push_back(field_meta->len());
    LOG_INFO("Construct attr info: attr(field) length = %d", field_meta->len());
  }

  // 初始化BplusTreeIndex内部的handler（关键位置） --> internal_max_size和leaf_max_size使用默认值 ???
  RC rc = index_handler_.create(table->db()->log_handler(), bpm, file_name, attr_type, attr_length);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, rc:%s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create index, file_name:%s, index:%s",
    file_name, index_meta.name());
  return RC::SUCCESS;
}

// file_name = miniob/db/sys/test-test_index.index
RC BplusTreeIndex::open(
    Table *table, const char *file_name, const IndexMeta &index_meta, const vector<const FieldMeta *> &field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been initedd before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }

  // 将元数据加载进属性中
  Index::init(index_meta, field_metas);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  // 具体打开操作交由handler完成
  RC                 rc  = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name: %s, index: %s, rc: %s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open index, file_name:%s, index:%s",
    file_name, index_meta.name());
  return RC::SUCCESS;
}

RC BplusTreeIndex::close()
{
  if (inited_) {
    LOG_INFO("Begin to close index, index:%s", index_meta_.name());
    index_handler_.close();
    inited_ = false;
  }
  LOG_INFO("Successfully close index.");
  return RC::SUCCESS;
}

RC BplusTreeIndex::insert_entry(const char *record, const RID *rid)
{
  // return index_handler_.insert_entry(record + field_meta_.offset(), rid);

  // 构建复合键 --> 字节级存储
  int total_key_length = 0;
  int count = 1;
  for (const FieldMeta *field_meta : field_metas_) {
    if (nullptr == field_meta) {
      LOG_ERROR("Found null field meta in index %s", index_meta_.name());
      return RC::INVALID_ARGUMENT;
    }

    int delta_len = 0;
    try {
      LOG_INFO("Get attr %d's length = %d when computing total key length in BplusTreeIndex::insert_entry()", count, field_meta->len());
      delta_len = field_meta->len();
    } catch (exception &e) {
      LOG_ERROR(e.what());
      return RC::INVALID_ARGUMENT;
    }
    total_key_length += delta_len;
    count++;

    // 防止累加溢出
    if (total_key_length > INT32_MAX) {
      LOG_ERROR("Total key length overflow for index %s, total = %d", index_meta_.name(), total_key_length);
      return RC::INVALID_ARGUMENT;
    }
  }

  // 这里传进去的不是record指向的字段 ！！！
  vector<char> composite_key;
  composite_key.reserve(total_key_length);

  for (const FieldMeta *field_meta : field_metas_) {
    // offset：获取record中对应field的数据位置
    const char *field_data = record + field_meta->offset();
    composite_key.insert(composite_key.end(), field_data, field_data + field_meta->len());
  }

  // data()获得指向底层数组的指针
  return index_handler_.insert_entry(composite_key.data(), rid);
}

RC BplusTreeIndex::delete_entry(const char *record, const RID *rid)
{
  // return index_handler_.delete_entry(record + field_meta_.offset(), rid);
  // 构建复合键
  int32_t total_key_length = 0;
  int count = 1;
  for (const FieldMeta *field_meta : field_metas_) {
    if (nullptr == field_meta) {
      LOG_WARN("Found null field meta in index %s", index_meta_.name());
      return RC::INTERNAL;
    }
    LOG_INFO("Get attr %d's length = %d when computing total key length in BplusTreeIndex::delete_entry()", count, field_meta->len());
    total_key_length += field_meta->len();
    count++;

    // 防止累加溢出
    if (total_key_length > INT32_MAX) {
      LOG_ERROR("Total key length overflow for index %s, total = %d", index_meta_.name(), total_key_length);
      return RC::INVALID_ARGUMENT;
    }
  }

  vector<char> composite_key;
  composite_key.reserve(total_key_length);

  for (const FieldMeta *field_meta : field_metas_) {
    // offset：获取record中对应field的数据位置
    const char *field_data = record + field_meta->offset();
    composite_key.insert(composite_key.end(), field_data, field_data + field_meta->len());
  }

  return index_handler_.delete_entry(composite_key.data(), rid);
}

IndexScanner *BplusTreeIndex::create_scanner(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  BplusTreeIndexScanner *index_scanner = new BplusTreeIndexScanner(index_handler_);
  RC rc = index_scanner->open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open index scanner. rc=%d:%s", rc, strrc(rc));
    delete index_scanner;
    return nullptr;
  }
  return index_scanner;
}

RC BplusTreeIndex::sync() { return index_handler_.sync(); }

////////////////////////////////////////////////////////////////////////////////
BplusTreeIndexScanner::BplusTreeIndexScanner(BplusTreeHandler &tree_handler) : tree_scanner_(tree_handler) {}

BplusTreeIndexScanner::~BplusTreeIndexScanner() noexcept { tree_scanner_.close(); }

RC BplusTreeIndexScanner::open(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  return tree_scanner_.open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
}

RC BplusTreeIndexScanner::next_entry(RID *rid) { return tree_scanner_.next_entry(*rid); }

RC BplusTreeIndexScanner::destroy()
{
  delete this;
  return RC::SUCCESS;
}

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

BplusTreeIndex::~BplusTreeIndex() noexcept { close(); }

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, vector<const FieldMeta *> field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  /* 初始化索引元数据，以及各个索引字段的元数据 */
  Index::init(index_meta, field_metas);

  /* 构造BplusTreeHandler */
  BufferPoolManager &bpm = table->db()->buffer_pool_manager();

  vector<AttrType> attr_types;
  vector<int> attr_lengths;
  attr_types.reserve(field_metas.size());
  attr_lengths.reserve(field_metas.size());
  for(const FieldMeta *field_meta: field_metas) {
    attr_types.emplace_back(field_meta->type());
    attr_lengths.emplace_back(field_meta->len());
  }

  RC rc = index_handler_.create(table->db()->log_handler(), bpm, file_name, attr_types, attr_lengths);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, rc:%s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  /* 完成其余信息的初始化 */
  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, vector<const FieldMeta *> field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been initedd before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_metas);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name:%s, index:%s, rc:%s",
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

/* 仅拼接字段信息 */
RC BplusTreeIndex::insert_entry(const Record &record, const RID *rid)
{
  bool is_null[field_metas_.size()];
  int bitmap_length = sizeof(bool) * field_metas_.size();

  int total_attr_length = 0;
  for(int i = 0; i < field_metas_.size(); i++) {
    const FieldMeta *field_meta = field_metas_[i];
    // 应当是以字节为单位
    total_attr_length += field_meta->len();
    is_null[i] = record.get_null_information(field_meta->field_id());
  }

  // 构建user_key向下传递 --> 下层全部使用memcpy，可以先开一个栈上的数组
  int  key_buf_len = total_attr_length + bitmap_length + sizeof(RID);
  char key_buf[key_buf_len];
  memset(key_buf, 0, key_buf_len);
  
  int  offset = 0;
  // fields不一定是连续的字段，只能说整体上是有序的
  for (int i = 0; i < field_metas_.size(); i++) {
    const FieldMeta *field_meta = field_metas_[i];
    const char      *field_data = record.data() + field_meta->offset();

    memcpy(key_buf + offset, field_data, field_meta->len());
    offset += field_meta->len();
  }

  // rid在后面进行复制
  memcpy(key_buf + offset, is_null, sizeof(is_null));

  return index_handler_.insert_entry(key_buf, rid);
}

RC BplusTreeIndex::delete_entry(const char *record, const RID *rid)
{
  return index_handler_.delete_entry(record + field_meta_.offset(), rid);
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

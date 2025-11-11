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
#include <cstring>
#include <climits>
#include <cfloat>

BplusTreeIndex::~BplusTreeIndex() noexcept { close(); }

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_meta);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.create(table->db()->log_handler(), bpm, file_name, field_meta.type(), field_meta.len());
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, const vector<const FieldMeta *> &fields_meta)
{
  if (inited_) {
    LOG_WARN("Failed to create composite index due to the index has been created before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, fields_meta);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  // 对于复合索引，使用 CHARS 类型，长度为所有字段长度的总和
  int total_key_length = calculate_key_length();
  RC rc = index_handler_.create(table->db()->log_handler(), bpm, file_name, AttrType::CHARS, total_key_length);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create composite index_handler, file_name:%s, index:%s, rc:%s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create composite index, file_name:%s, index:%s, field_count:%zu",
    file_name, index_meta.name(), fields_meta.size());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been initedd before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_meta);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, const vector<const FieldMeta *> &fields_meta)
{
  if (inited_) {
    LOG_WARN("Failed to open composite index due to the index has been initedd before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, fields_meta);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open composite index_handler, file_name:%s, index:%s, rc:%s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open composite index, file_name:%s, index:%s, field_count:%zu",
    file_name, index_meta.name(), fields_meta.size());
  return RC::SUCCESS;
}

RC BplusTreeIndex::close()
{
  if (inited_) {
    LOG_INFO("Begin to close index, index:%s, field:%s", index_meta_.name(), index_meta_.field());
    index_handler_.close();
    inited_ = false;
  }
  LOG_INFO("Successfully close index.");
  return RC::SUCCESS;
}

RC BplusTreeIndex::insert_entry(const char *record, const RID *rid)
{
  // 如果是复合索引，构建复合键
  if (fields_meta_.size() > 1) {
    int key_length = calculate_key_length();
    char *key_buffer = new char[key_length];
    build_composite_key(record, key_buffer);
    RC rc = index_handler_.insert_entry(key_buffer, rid);
    delete[] key_buffer;
    return rc;
  } else {
    // 单字段索引，向后兼容
    return index_handler_.insert_entry(record + field_meta_.offset(), rid);
  }
}

RC BplusTreeIndex::delete_entry(const char *record, const RID *rid)
{
  // 如果是复合索引，构建复合键
  if (fields_meta_.size() > 1) {
    int key_length = calculate_key_length();
    char *key_buffer = new char[key_length];
    build_composite_key(record, key_buffer);
    RC rc = index_handler_.delete_entry(key_buffer, rid);
    delete[] key_buffer;
    return rc;
  } else {
    // 单字段索引，向后兼容
    return index_handler_.delete_entry(record + field_meta_.offset(), rid);
  }
}

RC BplusTreeIndex::get_entry(const char *user_key, int key_len, list<RID> &rids)
{
  return index_handler_.get_entry(user_key, key_len, rids);
}

IndexScanner *BplusTreeIndex::create_scanner(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  // 如果是复合索引且传入的键长度小于完整键长度，需要扩展键
  int full_key_length = calculate_key_length();
  const char *final_left_key = left_key;
  int final_left_len = left_len;
  const char *final_right_key = right_key;
  int final_right_len = right_len;
  
  char *expanded_left_key = nullptr;
  char *expanded_right_key = nullptr;
  
  if (fields_meta_.size() > 1) {
    // 复合索引
    if (left_key != nullptr && left_len < full_key_length) {
      // 扩展左边界键：根据每个字段的类型填充最小值
      expanded_left_key = new char[full_key_length];
      memcpy(expanded_left_key, left_key, left_len);
      
      int offset = left_len;
      // 找到第一个未完全填充的字段
      int accumulated_len = 0;
      size_t start_field_idx = 0;
      for (size_t i = 0; i < fields_meta_.size(); i++) {
        accumulated_len += fields_meta_[i]->len();
        if (accumulated_len > left_len) {
          start_field_idx = i;
          break;
        }
      }
      
      // 从找到的字段开始填充最小值
      for (size_t j = start_field_idx; j < fields_meta_.size(); j++) {
        const FieldMeta *field_meta = fields_meta_[j];
        AttrType field_type = field_meta->type();
        int field_len = field_meta->len();
        
        // 根据字段类型填充最小值
        if (field_type == AttrType::INTS) {
          // 有符号整数的最小值：INT_MIN = -2147483648 = 0x80000000
          int min_int = INT_MIN;
          memcpy(expanded_left_key + offset, &min_int, field_len);
        } else if (field_type == AttrType::FLOATS) {
          // 浮点数的最小值：-FLT_MAX
          float min_float = -FLT_MAX;
          memcpy(expanded_left_key + offset, &min_float, field_len);
        } else {
          // 其他类型（CHARS, DATES等）用全0填充
          memset(expanded_left_key + offset, 0, field_len);
        }
        offset += field_len;
      }
      
      final_left_key = expanded_left_key;
      final_left_len = full_key_length;
    }
    
    if (right_key != nullptr && right_len < full_key_length) {
      // 扩展右边界键：根据每个字段的类型填充最大值
      expanded_right_key = new char[full_key_length];
      memcpy(expanded_right_key, right_key, right_len);
      
      int offset = right_len;
      // 找到第一个未完全填充的字段
      int accumulated_len = 0;
      size_t start_field_idx = 0;
      for (size_t i = 0; i < fields_meta_.size(); i++) {
        accumulated_len += fields_meta_[i]->len();
        if (accumulated_len > right_len) {
          start_field_idx = i;
          break;
        }
      }
      
      // 从找到的字段开始填充最大值
      for (size_t j = start_field_idx; j < fields_meta_.size(); j++) {
        const FieldMeta *field_meta = fields_meta_[j];
        AttrType field_type = field_meta->type();
        int field_len = field_meta->len();
        
        // 根据字段类型填充最大值
        if (field_type == AttrType::INTS) {
          // 有符号整数的最大值：INT_MAX = 2147483647 = 0x7FFFFFFF
          int max_int = INT_MAX;
          memcpy(expanded_right_key + offset, &max_int, field_len);
        } else if (field_type == AttrType::FLOATS) {
          // 浮点数的最大值：FLT_MAX
          float max_float = FLT_MAX;
          memcpy(expanded_right_key + offset, &max_float, field_len);
        } else {
          // 其他类型（CHARS, DATES等）用全0xFF填充
          memset(expanded_right_key + offset, 0xFF, field_len);
        }
        offset += field_len;
      }
      
      final_right_key = expanded_right_key;
      final_right_len = full_key_length;
      
      // 对于部分键查询，确保右边界键大于左边界键，避免触发精确匹配检查
      // 如果左右边界键相等，BplusTreeScanner 会进行精确匹配检查，导致部分键查询失败
      if (left_key != nullptr && left_len == right_len && 
          memcmp(left_key, right_key, left_len) == 0) {
        // 左右边界键相等，说明这是等值查询的部分键
        // 我们需要确保扩展后的右边界键大于左边界键
        // 由于我们已经用最大值填充了右边界，所以应该已经满足条件
        // 但为了确保，我们可以稍微调整右边界键的最后一个字节
        if (expanded_right_key != nullptr && expanded_left_key != nullptr) {
          // 检查扩展后的键是否相等
          if (memcmp(expanded_left_key, expanded_right_key, full_key_length) == 0) {
            // 如果相等，我们需要让右边界键稍微大一点
            // 在最后一个字节加1（如果可能的话）
            if (full_key_length > 0) {
              unsigned char last_byte = static_cast<unsigned char>(expanded_right_key[full_key_length - 1]);
              if (last_byte < 0xFF) {
                expanded_right_key[full_key_length - 1] = last_byte + 1;
              } else {
                // 如果已经是0xFF，我们需要在更前面的字节加1
                // 但为了简单，我们可以在前面添加一个字节（但这会改变键长度）
                // 实际上，由于我们填充的是最大值，左右边界键不应该相等
                // 如果相等，说明填充逻辑有问题
                LOG_WARN("Expanded left and right keys are equal for partial key query");
              }
            }
          }
        }
      }
    }
  }
  
  BplusTreeIndexScanner *index_scanner = new BplusTreeIndexScanner(index_handler_);
  RC rc = index_scanner->open(final_left_key, final_left_len, left_inclusive, final_right_key, final_right_len, right_inclusive);
  
  // 清理临时分配的内存
  if (expanded_left_key != nullptr) {
    delete[] expanded_left_key;
  }
  if (expanded_right_key != nullptr) {
    delete[] expanded_right_key;
  }
  
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open index scanner. rc=%d:%s", rc, strrc(rc));
    delete index_scanner;
    return nullptr;
  }
  return index_scanner;
}

RC BplusTreeIndex::sync() { return index_handler_.sync(); }

int BplusTreeIndex::build_composite_key(const char *record, char *key_buffer) const
{
  int offset = 0;
  for (const FieldMeta *field_meta : fields_meta_) {
    const char *field_data = record + field_meta->offset();
    int field_len = field_meta->len();
    memcpy(key_buffer + offset, field_data, field_len);
    offset += field_len;
  }
  return offset;
}

int BplusTreeIndex::calculate_key_length() const
{
  int total_length = 0;
  for (const FieldMeta *field_meta : fields_meta_) {
    total_length += field_meta->len();
  }
  return total_length;
}

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

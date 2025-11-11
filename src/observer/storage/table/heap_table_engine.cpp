/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/table/heap_table_engine.h"
#include "storage/record/heap_record_scanner.h"
#include "common/log/log.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include <unordered_map>
#include <cstring>


HeapTableEngine::~HeapTableEngine()
{
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }

  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  for (vector<Index *>::iterator it = indexes_.begin(); it != indexes_.end(); ++it) {
    Index *index = *it;
    delete index;
  }
  indexes_.clear();

  LOG_INFO("Table has been closed: %s", table_meta_->name());
}

/* 原先在make_record中定义的管理null值的bitmap此后被销毁 */
RC HeapTableEngine::insert_record(Record &record)
{
  LOG_TRACE("HeapTableEngine::insert_record() is called");
  RC rc = RC::SUCCESS;
  // 并没有实质上使用record --> record_file_handler中的接口暂时都还是Unimplemented
  rc    = record_handler_->insert_record(record.data(), table_meta_->record_size(), &record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_->name(), strrc(rc));
    return rc;
  }

  rc = insert_entry_of_indexes(record.data(), record.rid(), &record);
  if (rc != RC::SUCCESS) {  // 可能出现了键值重复
    RC rc2 = delete_entry_of_indexes(record.data(), record.rid(), false /*error_on_not_exists*/);
    if (rc2 != RC::SUCCESS) {
      LOG_ERROR("Failed to rollback index data when insert index entries failed. table name=%s, rc=%d:%s",
                table_meta_->name(), rc2, strrc(rc2));
    }
    rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                table_meta_->name(), rc2, strrc(rc2));
    }
  }
  return rc;
}

RC HeapTableEngine::insert_chunk(const Chunk& chunk)
{
  RC rc = RC::SUCCESS;
  rc    = record_handler_->insert_chunk(chunk, table_meta_->record_size());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert chunk failed. table name=%s, rc=%s", table_meta_->name(), strrc(rc));
    return rc;
  }

  // TODO: insert chunk support update index
  return rc;
}

RC HeapTableEngine::visit_record(const RID &rid, function<bool(Record &)> visitor)
{
  return record_handler_->visit_record(rid, visitor);
}

RC HeapTableEngine::get_record(const RID &rid, Record &record)
{
  RC rc = record_handler_->get_record(rid, record);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to visit record. rid=%s, table=%s, rc=%s", rid.to_string().c_str(), table_meta_->name(), strrc(rc));
    return rc;
  }

  return rc;
}

RC HeapTableEngine::delete_record(const Record &record)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record.data(), &record.rid());
    ASSERT(RC::SUCCESS == rc, 
           "failed to delete entry from index. table name=%s, index name=%s, rid=%s, rc=%s",
           table_meta_->name(), index->index_meta().name(), record.rid().to_string().c_str(), strrc(rc));
  }
  rc = record_handler_->delete_record(&record.rid());
  return rc;
}

RC HeapTableEngine::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  scanner = new HeapRecordScanner(table_, *data_buffer_pool_, trx, db_->log_handler(), mode, nullptr);
  RC rc = scanner->open_scan();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC HeapTableEngine::get_chunk_scanner(ChunkFileScanner &scanner, Trx *trx, ReadWriteMode mode)
{
  RC rc = scanner.open_scan_chunk(table_, *data_buffer_pool_, db_->log_handler(), mode);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC HeapTableEngine::create_index(Trx *trx, const FieldMeta *field_meta, const char *index_name, bool is_unique)
{
  if (common::is_blank(index_name) || nullptr == field_meta) {
    LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or attribute_name is blank", table_meta_->name());
    return RC::INVALID_ARGUMENT;
  }

  IndexMeta new_index_meta;

  RC rc = new_index_meta.init(index_name, *field_meta, is_unique);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Failed to init IndexMeta in table:%s, index_name:%s, field_name:%s", 
             table_meta_->name(), index_name, field_meta->name());
    return rc;
  }

  // 如果是唯一索引，先检查现有数据是否有重复值
  if (is_unique) {
    RecordScanner *check_scanner = nullptr;
    rc = get_record_scanner(check_scanner, trx, ReadWriteMode::READ_ONLY);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create scanner for uniqueness check. table=%s, index=%s, rc=%s", 
               table_meta_->name(), index_name, strrc(rc));
      return rc;
    }

    unordered_map<string, RID> key_to_rid;  // 用于检查重复键值
    Record record;
    while (OB_SUCC(rc = check_scanner->next(record))) {
      // 检查字段是否为 NULL，如果是 NULL，则跳过唯一性检查（SQL 标准允许多个 NULL）
      // field_id() 返回的是用户字段索引，需要加上 sys_field_num() 的偏移
      // 从持久化的 bitmap 中读取 NULL 信息
      int bitmap_index = field_meta->field_id() + table_meta_->sys_field_num();
      bool *bitmap = reinterpret_cast<bool *>(const_cast<char *>(record.data()) + table_meta_->fields_record_size());
      bool is_field_null = (bitmap_index >= 0 && bitmap_index < table_meta_->field_num()) ? bitmap[bitmap_index] : false;
      
      if (!is_field_null) {
        const char *key = record.data() + field_meta->offset();
        string key_str(key, field_meta->len());
        
        auto it = key_to_rid.find(key_str);
        if (it != key_to_rid.end()) {
          // 发现重复键值
          LOG_WARN("duplicate key value found while creating unique index. table=%s, index=%s", 
                   table_meta_->name(), index_name);
          check_scanner->close_scan();
          delete check_scanner;
          return RC::RECORD_DUPLICATE_KEY;
        }
        key_to_rid[key_str] = record.rid();
      }
    }
    if (rc != RC::RECORD_EOF) {
      check_scanner->close_scan();
      delete check_scanner;
      LOG_WARN("failed to scan records for uniqueness check. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      return rc;
    }
    check_scanner->close_scan();
    delete check_scanner;
  }

  // 创建索引相关数据
  BplusTreeIndex *index      = new BplusTreeIndex();
  string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_name);

  rc = index->create(table_, index_file.c_str(), new_index_meta, *field_meta);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create bplus tree index. file name=%s, rc=%d:%s", index_file.c_str(), rc, strrc(rc));
    return rc;
  }

  // 遍历当前的所有数据，插入这个索引
  RecordScanner *scanner = nullptr;
  rc = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create scanner while creating index. table=%s, index=%s, rc=%s", 
             table_meta_->name(), index_name, strrc(rc));
    delete index;
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner->next(record))) {
    rc = index->insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      scanner->close_scan();
      delete scanner;
      delete index;
      return rc;
    }
  }
  if (RC::RECORD_EOF == rc) {
    rc = RC::SUCCESS;
  } else {
    LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
             table_meta_->name(), index_name, strrc(rc));
    return rc;
  }
  scanner->close_scan();
  delete scanner;
  LOG_INFO("inserted all records into new index. table=%s, index=%s", table_meta_->name(), index_name);

  indexes_.push_back(index);

  /// 接下来将这个索引放到表的元数据中
  TableMeta new_table_meta(*table_meta_);
  rc = new_table_meta.add_index(new_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add index (%s) on table (%s). error=%d:%s", index_name, table_meta_->name(), rc, strrc(rc));
    return rc;
  }

  /// 内存中有一份元数据，磁盘文件也有一份元数据。修改磁盘文件时，先创建一个临时文件，写入完成后再rename为正式文件
  /// 这样可以防止文件内容不完整
  // 创建元数据临时文件
  string  tmp_file = table_meta_file(db_->path().c_str(), table_meta_->name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;  // 创建索引中途出错，要做还原操作
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  // 覆盖原始元数据文件
  string meta_file = table_meta_file(db_->path().c_str(), table_meta_->name());

  int ret = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating index (%s) on table (%s). "
              "system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), index_name, table_meta_->name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_->swap(new_table_meta);

  LOG_INFO("Successfully added a new index (%s) on the table (%s)", index_name, table_meta_->name());
  return rc;
}

RC HeapTableEngine::create_index(Trx *trx, const vector<const FieldMeta *> &fields_meta, const char *index_name, bool is_unique)
{
  if (common::is_blank(index_name) || fields_meta.empty()) {
    LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or no fields provided", table_meta_->name());
    return RC::INVALID_ARGUMENT;
  }

  IndexMeta new_index_meta;
  RC rc = new_index_meta.init(index_name, fields_meta, is_unique);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Failed to init IndexMeta in table:%s, index_name:%s", 
             table_meta_->name(), index_name);
    return rc;
  }

  // 如果是唯一索引，先检查现有数据是否有重复值
  if (is_unique) {
    RecordScanner *check_scanner = nullptr;
    rc = get_record_scanner(check_scanner, trx, ReadWriteMode::READ_ONLY);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create scanner for uniqueness check. table=%s, index=%s, rc=%s", 
               table_meta_->name(), index_name, strrc(rc));
      return rc;
    }

    unordered_map<string, RID> key_to_rid;  // 用于检查重复键值
    Record record;
    while (OB_SUCC(rc = check_scanner->next(record))) {
      // 检查所有字段是否都为 NULL（如果任何一个字段为 NULL，则跳过唯一性检查）
      bool has_null = false;
      bool *bitmap = reinterpret_cast<bool *>(const_cast<char *>(record.data()) + table_meta_->fields_record_size());
      
      for (const FieldMeta *field_meta : fields_meta) {
        int bitmap_index = field_meta->field_id() + table_meta_->sys_field_num();
        bool is_field_null = (bitmap_index >= 0 && bitmap_index < table_meta_->field_num()) ? bitmap[bitmap_index] : false;
        if (is_field_null) {
          has_null = true;
          break;
        }
      }
      
      if (!has_null) {
        // 构建复合键
        int total_key_length = 0;
        for (const FieldMeta *field_meta : fields_meta) {
          total_key_length += field_meta->len();
        }
        string key_str;
        key_str.reserve(total_key_length);
        for (const FieldMeta *field_meta : fields_meta) {
          const char *field_data = record.data() + field_meta->offset();
          key_str.append(field_data, field_meta->len());
        }
        
        auto it = key_to_rid.find(key_str);
        if (it != key_to_rid.end()) {
          // 发现重复键值
          LOG_WARN("duplicate key value found while creating unique composite index. table=%s, index=%s", 
                   table_meta_->name(), index_name);
          check_scanner->close_scan();
          delete check_scanner;
          return RC::RECORD_DUPLICATE_KEY;
        }
        key_to_rid[key_str] = record.rid();
      }
    }
    if (rc != RC::RECORD_EOF) {
      check_scanner->close_scan();
      delete check_scanner;
      LOG_WARN("failed to scan records for uniqueness check. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      return rc;
    }
    check_scanner->close_scan();
    delete check_scanner;
  }

  // 创建索引相关数据
  BplusTreeIndex *index      = new BplusTreeIndex();
  string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_name);

  rc = index->create(table_, index_file.c_str(), new_index_meta, fields_meta);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create bplus tree composite index. file name=%s, rc=%d:%s", index_file.c_str(), rc, strrc(rc));
    return rc;
  }

  // 遍历当前的所有数据，插入这个索引
  RecordScanner *scanner = nullptr;
  rc = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create scanner while creating index. table=%s, index=%s, rc=%s", 
             table_meta_->name(), index_name, strrc(rc));
    delete index;
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner->next(record))) {
    rc = index->insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      scanner->close_scan();
      delete scanner;
      delete index;
      return rc;
    }
  }
  if (RC::RECORD_EOF == rc) {
    rc = RC::SUCCESS;
  } else {
    LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
             table_meta_->name(), index_name, strrc(rc));
    return rc;
  }
  scanner->close_scan();
  delete scanner;
  LOG_INFO("inserted all records into new composite index. table=%s, index=%s", table_meta_->name(), index_name);

  indexes_.push_back(index);

  /// 接下来将这个索引放到表的元数据中
  TableMeta new_table_meta(*table_meta_);
  rc = new_table_meta.add_index(new_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add index (%s) on table (%s). error=%d:%s", index_name, table_meta_->name(), rc, strrc(rc));
    return rc;
  }

  /// 内存中有一份元数据，磁盘文件也有一份元数据。修改磁盘文件时，先创建一个临时文件，写入完成后再rename为正式文件
  /// 这样可以防止文件内容不完整
  // 创建元数据临时文件
  string  tmp_file = table_meta_file(db_->path().c_str(), table_meta_->name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;  // 创建索引中途出错，要做还原操作
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  // 覆盖原始元数据文件
  string meta_file = table_meta_file(db_->path().c_str(), table_meta_->name());

  int ret = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating index (%s) on table (%s). "
              "system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), index_name, table_meta_->name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_->swap(new_table_meta);

  // 在 swap 之后，需要从新的 table_meta_ 中重新获取 FieldMeta 指针并更新索引
  // 因为 swap 后，原来的 FieldMeta 对象可能被销毁了
  vector<const FieldMeta *> refreshed_fields_meta;
  for (const string &field_name : new_index_meta.fields()) {
    const FieldMeta *field_meta = table_meta_->field(field_name.c_str());
    if (field_meta == nullptr) {
      LOG_ERROR("Failed to find field %s in table %s after swap", field_name.c_str(), table_meta_->name());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    refreshed_fields_meta.push_back(field_meta);
  }
  // 刷新索引的字段指针
  rc = index->refresh_fields_meta(refreshed_fields_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to refresh fields meta for index %s", index_name);
    return rc;
  }

  LOG_INFO("Successfully added a new composite index (%s) on the table (%s)", index_name, table_meta_->name());
  return rc;
}

RC HeapTableEngine::insert_entry_of_indexes(const char *record, const RID &rid, const Record *record_obj)
{
  LOG_TRACE("HeapTableEngine::insert_entry_of_indexes() is called");
  RC rc = RC::SUCCESS;
  bool *bitmap = reinterpret_cast<bool *>(const_cast<char *>(record) + table_meta_->fields_record_size());
  
  for (Index *index : indexes_) {
    const vector<string> &index_fields = index->index_meta().fields();
    
    // 检查所有索引字段是否都为 NULL（如果任何一个字段为 NULL，则跳过索引插入）
    // 重要：必须确保 fields_meta 的长度与索引内部的 fields_meta_ 一致，否则键值长度不匹配
    bool has_null = false;
    vector<const FieldMeta *> fields_meta;
    for (const string &field_name : index_fields) {
      const FieldMeta *field_meta = table_meta_->field(field_name.c_str());
      if (field_meta == nullptr) {
        // 字段不存在，这是不应该发生的，但为了健壮性，跳过这个索引
        // 如果跳过字段，会导致 fields_meta 长度与 fields_meta_ 不一致，键值长度不匹配
        LOG_WARN("Field %s not found in table %s for index %s, skipping index insertion", 
                 field_name.c_str(), table_meta_->name(), index->index_meta().name());
        fields_meta.clear();
        break;
      }
      fields_meta.push_back(field_meta);
      int bitmap_index = field_meta->field_id() + table_meta_->sys_field_num();
      bool is_field_null = (bitmap_index >= 0 && bitmap_index < table_meta_->field_num()) ? bitmap[bitmap_index] : false;
      if (is_field_null) {
        has_null = true;
        break;
      }
    }
    
    // 必须确保 fields_meta 的长度与 index_fields 一致，否则键值长度与索引期望的 attr_length 不匹配
    // 这会导致 fix_user_key 用 0 填充键值，造成键值不匹配，唯一性检查误判
    if (has_null || fields_meta.empty() || fields_meta.size() != index_fields.size()) {
      LOG_TRACE("One or more fields are NULL or missing, skipping index insertion for index %s", 
                index->index_meta().name());
      continue;
    }
    
    // 如果是唯一索引，先检查是否已存在相同的键值
    // 注意：insert_entry 内部检查的是完整键（user_key + rid），无法检测唯一性约束违反
    // 所以需要预先检查用户键值是否重复
    if (index->index_meta().is_unique()) {
      // 构建复合键：使用 fields_meta 的顺序（已经按照 index_fields 的顺序构建）
      int total_key_length = 0;
      for (const FieldMeta *field_meta : fields_meta) {
        total_key_length += field_meta->len();
      }
      char *key_buffer = new char[total_key_length];
      int offset = 0;
      for (const FieldMeta *field_meta : fields_meta) {
        const char *field_data = record + field_meta->offset();
        memcpy(key_buffer + offset, field_data, field_meta->len());
        offset += field_meta->len();
      }
      
      // 调试：打印键值内容（仅用于调试）
      LOG_DEBUG("Checking unique constraint. table=%s, index=%s, key_len=%d", 
                table_meta_->name(), index->index_meta().name(), total_key_length);
      
      list<RID> existing_rids;
      rc = index->get_entry(key_buffer, total_key_length, existing_rids);
      delete[] key_buffer;
      if (rc == RC::SUCCESS && !existing_rids.empty()) {
        // 已存在相同的键值，违反唯一性约束
        LOG_WARN("duplicate key value violates unique constraint. table=%s, index=%s, existing_rids_count=%zu", 
                 table_meta_->name(), index->index_meta().name(), existing_rids.size());
        return RC::RECORD_DUPLICATE_KEY;
      }
    }
    
    // 插入索引条目（非 NULL 值）
    rc = index->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      break;
    }
  }
  return rc;
}

RC HeapTableEngine::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists)
{
  RC rc = RC::SUCCESS;
  bool *bitmap = reinterpret_cast<bool *>(const_cast<char *>(record) + table_meta_->fields_record_size());
  
  for (Index *index : indexes_) {
    const vector<string> &index_fields = index->index_meta().fields();
    
    // 检查所有索引字段是否都为 NULL（如果任何一个字段为 NULL，则跳过索引删除）
    bool has_null = false;
    for (const string &field_name : index_fields) {
      const FieldMeta *field_meta = table_meta_->field(field_name.c_str());
      if (field_meta == nullptr) {
        has_null = true;
        break;
      }
      int bitmap_index = field_meta->field_id() + table_meta_->sys_field_num();
      bool is_field_null = (bitmap_index >= 0 && bitmap_index < table_meta_->field_num()) ? bitmap[bitmap_index] : false;
      if (is_field_null) {
        has_null = true;
        break;
      }
    }
    
    if (has_null) {
      LOG_TRACE("One or more fields are NULL, skipping index deletion for index %s", 
                index->index_meta().name());
      continue;
    }
    
    rc = index->delete_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      if (rc != RC::RECORD_INVALID_KEY || !error_on_not_exists) {
        break;
      }
    }
  }
  return rc;
}

RC HeapTableEngine::sync()
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush index's pages. table=%s, index=%s, rc=%d:%s",
          table_meta_->name(),
          index->index_meta().name(),
          rc,
          strrc(rc));
      return rc;
    }
  }

  rc = data_buffer_pool_->flush_all_pages();
  LOG_INFO("Sync table over. table=%s", table_meta_->name());
  return rc;
}

Index *HeapTableEngine::find_index(const char *index_name) const
{
  for (Index *index : indexes_) {
    if (0 == strcmp(index->index_meta().name(), index_name)) {
      return index;
    }
  }
  return nullptr;
}
Index *HeapTableEngine::find_index_by_field(const char *field_name) const
{
  const IndexMeta *index_meta = table_meta_->find_index_by_field(field_name);
  if (index_meta != nullptr) {
    return this->find_index(index_meta->name());
  }
  return nullptr;
}

RC HeapTableEngine::init()
{
  string data_file = table_data_file(db_->path().c_str(), table_meta_->name());

  BufferPoolManager &bpm = db_->buffer_pool_manager();
  RC                 rc  = bpm.open_file(db_->log_handler(), data_file.c_str(), data_buffer_pool_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open disk buffer pool for file:%s. rc=%d:%s", data_file.c_str(), rc, strrc(rc));
    return rc;
  }

  record_handler_ = new RecordFileHandler(table_meta_->storage_format());

  rc = record_handler_->init(*data_buffer_pool_, db_->log_handler(), table_meta_, table_->lob_handler_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to init record handler. rc=%s", strrc(rc));
    delete record_handler_;
    record_handler_ = nullptr;
    return rc;
  }

  return rc;
}

// 创建field and index
RC HeapTableEngine::open()
{
  RC rc = RC::SUCCESS;
  init();
  const int index_num = table_meta_->index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_->index(i);
    BplusTreeIndex *index      = new BplusTreeIndex();
    string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_meta->name());

    // 检查是单字段索引还是复合索引
    if (index_meta->field_count() > 1) {
      // 复合索引：获取所有字段的元数据
      vector<const FieldMeta *> fields_meta;
      const vector<string> &field_names = index_meta->fields();
      for (const string &field_name : field_names) {
        const FieldMeta *field_meta = table_meta_->field(field_name.c_str());
        if (field_meta == nullptr) {
          LOG_ERROR("Found invalid index meta info which has a non-exists field. table=%s, index=%s, field=%s",
                    table_meta_->name(), index_meta->name(), field_name.c_str());
          delete index;
          return RC::INTERNAL;
        }
        fields_meta.push_back(field_meta);
      }
      rc = index->open(table_, index_file.c_str(), *index_meta, fields_meta);
    } else {
      // 单字段索引：向后兼容
      const FieldMeta *field_meta = table_meta_->field(index_meta->field());
      if (field_meta == nullptr) {
        LOG_ERROR("Found invalid index meta info which has a non-exists field. table=%s, index=%s, field=%s",
                  table_meta_->name(), index_meta->name(), index_meta->field());
        delete index;
        return RC::INTERNAL;
      }
      rc = index->open(table_, index_file.c_str(), *index_meta, *field_meta);
    }

    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("Failed to open index. table=%s, index=%s, file=%s, rc=%s",
                table_meta_->name(), index_meta->name(), index_file.c_str(), strrc(rc));
      // skip cleanup
      //  do all cleanup action in destructive Table function.
      return rc;
    }
    indexes_.push_back(index);
  }
  return rc;
}

RC HeapTableEngine::update_record_with_trx(const Record &old_record, const Record &new_record, Trx *trx)
{
  RC rc = RC::SUCCESS;
  
  // 1. 更新索引：先删除旧记录，再插入新记录
  bool *old_bitmap = reinterpret_cast<bool *>(const_cast<char *>(old_record.data()) + table_meta_->fields_record_size());
  bool *new_bitmap = reinterpret_cast<bool *>(const_cast<char *>(new_record.data()) + table_meta_->fields_record_size());
  
  for (Index *index : indexes_) {
    const vector<string> &index_fields = index->index_meta().fields();
    
    // 检查旧值和新值是否都为 NULL（如果任何一个字段为 NULL，则跳过索引操作）
    bool old_has_null = false;
    bool new_has_null = false;
    vector<const FieldMeta *> fields_meta;
    
    for (const string &field_name : index_fields) {
      const FieldMeta *field_meta = table_meta_->field(field_name.c_str());
      if (field_meta == nullptr) {
        continue;
      }
      fields_meta.push_back(field_meta);
      int bitmap_index = field_meta->field_id() + table_meta_->sys_field_num();
      bool old_is_null = (bitmap_index >= 0 && bitmap_index < table_meta_->field_num()) ? old_bitmap[bitmap_index] : false;
      bool new_is_null = (bitmap_index >= 0 && bitmap_index < table_meta_->field_num()) ? new_bitmap[bitmap_index] : false;
      if (old_is_null) {
        old_has_null = true;
      }
      if (new_is_null) {
        new_has_null = true;
      }
    }
    
    // 如果旧值不是 NULL，需要从索引中删除
    if (!old_has_null && !fields_meta.empty()) {
      rc = index->delete_entry(old_record.data(), &old_record.rid());
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to delete entry from index. table=%s, index=%s, rid=%s, rc=%s",
                 table_meta_->name(), index->index_meta().name(), 
                 old_record.rid().to_string().c_str(), strrc(rc));
        return rc;
      }
    }
    
    // 如果新值不是 NULL，需要插入到索引中
    if (!new_has_null && !fields_meta.empty()) {
      // 如果是唯一索引，先检查是否已存在相同的键值
      if (index->index_meta().is_unique()) {
        // 构建复合键（使用字符数组而不是 string，因为键可能包含二进制数据）
        int total_key_length = 0;
        for (const FieldMeta *field_meta : fields_meta) {
          total_key_length += field_meta->len();
        }
        char *key_buffer = new char[total_key_length];
        int offset = 0;
        for (const FieldMeta *field_meta : fields_meta) {
          const char *field_data = new_record.data() + field_meta->offset();
          memcpy(key_buffer + offset, field_data, field_meta->len());
          offset += field_meta->len();
        }
        
        list<RID> existing_rids;
        rc = index->get_entry(key_buffer, total_key_length, existing_rids);
        delete[] key_buffer;
        if (rc == RC::SUCCESS && !existing_rids.empty()) {
          // 检查是否是自己（如果只是更新了其他字段，但索引字段值没变）
          bool is_self = false;
          for (const RID &existing_rid : existing_rids) {
            if (existing_rid == new_record.rid()) {
              is_self = true;
              break;
            }
          }
          if (!is_self) {
            // 已存在相同的键值，违反唯一性约束
            LOG_WARN("duplicate key value violates unique constraint. table=%s, index=%s", 
                     table_meta_->name(), index->index_meta().name());
            return RC::RECORD_DUPLICATE_KEY;
          }
        }
      }
      
      rc = index->insert_entry(new_record.data(), &new_record.rid());
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to insert entry to index. table=%s, index=%s, rid=%s, rc=%s",
                 table_meta_->name(), index->index_meta().name(), 
                 new_record.rid().to_string().c_str(), strrc(rc));
        return rc;
      }
    }
  }
  
  // 2. 更新记录数据
  rc = record_handler_->delete_record(&old_record.rid());
  if (rc != RC::SUCCESS) {
      LOG_WARN("failed to delete old record. table=%s, index=%s, rid=%s, rc=%s",
               table_meta_->name(), old_record.rid().to_string().c_str(), strrc(rc));
      return rc;
  }
  // RID &rid = new_record.rid(); // 先获取引用
  rc = record_handler_->insert_record(new_record.data(), new_record.len(), const_cast<RID*>(&new_record.rid()));
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to update record. table=%s, rid=%s, rc=%s",
             table_meta_->name(), old_record.rid().to_string().c_str(), strrc(rc));
    return rc;
  }
  
  return RC::SUCCESS;
}
// 清除field and index --> 索引还unsupported
RC HeapTableEngine::close()
{
  // 关闭索引
  indexes_.clear();  

  // 关闭记录处理器
  if (record_handler_ != nullptr) {
    // 已经完整实现
    record_handler_->close();  
    delete record_handler_;
    record_handler_ = nullptr;
  }

  // // 关闭磁盘数据缓冲池 DiskDataPool --> 与table.cpp中的drop方法无冲突
  // if (data_buffer_pool_ != nullptr) {
  //   data_buffer_pool_->close_file();  
  //   delete data_buffer_pool_;
  //   data_buffer_pool_ = nullptr;
  // }

  LOG_INFO("Table has been closed: %s", table_meta_->name());
  return RC::SUCCESS;
}

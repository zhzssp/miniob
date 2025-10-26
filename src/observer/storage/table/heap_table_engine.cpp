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
RC HeapTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  rc    = record_handler_->insert_record(record.data(), table_meta_->record_size(), &record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_->name(), strrc(rc));
    return rc;
  }

  LOG_INFO("Table engine tries to insert record.");
  rc = insert_entry_of_indexes(record.data(), record.rid());
  // 可能出现了键值重复
  if (rc != RC::SUCCESS) { 
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

RC HeapTableEngine::insert_chunk(const Chunk &chunk)
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
  RC rc   = scanner->open_scan();
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

// Trx为事务接口
RC HeapTableEngine::create_index(Trx *trx, const vector<const FieldMeta *> &field_metas, const char *index_name)
{
  if (common::is_blank(index_name) || field_metas.empty()) {
    LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or field_metas is empty", table_meta_->name());
    return RC::INVALID_ARGUMENT;
  }

  // 细化存储field相关信息
  IndexMeta new_index_metas;

  // 指向table_meta_中的vector<FieldMeta>
  RC rc = new_index_metas.init(index_name, field_metas);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Failed to init IndexMeta in table: %s, index_name: %s", 
             table_meta_->name(), index_name);
    return rc;
  }
  else {
    LOG_INFO("Successfully initialize IndexMeta in table: %s, index_name: %s", 
            table_meta_->name(), index_name);
  }

  // 创建索引相关数据结构
  BplusTreeIndex *index      = new BplusTreeIndex();
  string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_name);

  // field_metas一直将引用向下传递 --> 使用的是new_index_metas, 仍然指向table_meta_中的vector<FieldMeta>
  rc = index->create(table_, index_file.c_str(), new_index_metas, field_metas);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create bplus tree index. file name=%s, rc=%d:%s", index_file.c_str(), rc, strrc(rc));
    return rc;
  }

  // 遍历当前的所有数据，插入这个索引
  RecordScanner *scanner = nullptr;
  rc                     = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create scanner while creating index. table=%s, index=%s, rc=%s", 
             table_meta_->name(), index_name, strrc(rc));
    return rc;
  }

  Record record;
  // 将扫描得到的record逐条插入到新建的索引中 
  while (OB_SUCC(rc = scanner->next(record))) {
    rc = index->insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
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

  // 添加到表的索引列表中
  indexes_.push_back(index);

  // 接下来将这个索引放到表的元数据中 --> 重新创建vector<FieldMeta>
  TableMeta new_table_meta(*table_meta_);
  rc = new_table_meta.add_index(new_index_metas);
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

  // 原先table_meta_的vector<FieldMeta>对象们被销毁 --> 前面保存的field_metas中的指针失效
  table_meta_->swap(new_table_meta);

  LOG_INFO("Successfully added a new index (%s) on the table (%s)", index_name, table_meta_->name());
  return rc;
}

RC HeapTableEngine::insert_entry_of_indexes(const char *record, const RID &rid)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    LOG_INFO("Insert record into Index %s. Location is table engine.", index->index_meta().name());
    // 理论上都是Index的子类BplusTreeIndex
    rc = index->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Cannot insert record into Index %s. Location is table engine.", index->index_meta().name());
      break;
    }
  }
  return rc;
}

RC HeapTableEngine::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
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
Index *HeapTableEngine::find_index_by_field(const vector<string> &field_names) const
{
  const IndexMeta *index_meta = table_meta_->find_index_by_field(field_names);
  if (index_meta != nullptr) {
    return this->find_index(index_meta->name());
  }
  else {
    LOG_WARN("Cannot find index by field");
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

// 根据表的元数据信息构建index --> 创建索引的B+树并初始化
RC HeapTableEngine::open()
{
  RC rc = RC::SUCCESS;
  init();
  const int index_num = table_meta_->index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_->index(i);

    // 构建字段元数据向量
    vector<const FieldMeta *> field_metas;
    for (int j = 0; j < index_meta->field_count(); j++) {
      const FieldMeta *field_meta = table_meta_->field(index_meta->field(j));
      if (field_meta == nullptr) {
        LOG_ERROR("Found invalid index meta info which has a non-exists field. table=%s, index=%s, field=%s",
                  table_meta_->name(), index_meta->name(), index_meta->field(j));
        // skip cleanup
        //  do all cleanup action in destructive Table function
        return RC::INTERNAL;
      }
      field_metas.push_back(field_meta);
    }

    BplusTreeIndex *index      = new BplusTreeIndex();
    // 设置.index索引文件路径
    string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_meta->name());

    rc = index->open(table_, index_file.c_str(), *index_meta, field_metas);
    // 没有正确打开索引文件等
    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("Table Engine failed to open index. table= %s, index= %s, file= %s, rc= %s",
                table_meta_->name(), index_meta->name(), index_file.c_str(), strrc(rc));
      // skip cleanup
      //  do all cleanup action in destructive Table function.
      return rc;
    }
    LOG_INFO("Table engine opens table's index %s successfully !", index_file.c_str());
    indexes_.push_back(index);
  }
  return rc;
}

RC HeapTableEngine::update_record_with_trx(const Record &old_record, const Record &new_record, Trx *trx)
{
  RC rc = RC::SUCCESS;

  // 1. 更新索引：先删除旧记录，再插入新记录
  for (Index *index : indexes_) {
    rc = index->delete_entry(old_record.data(), &old_record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to delete entry from index. table=%s, index=%s, rid=%s, rc=%s",
               table_meta_->name(), index->index_meta().name(), 
               old_record.rid().to_string().c_str(), strrc(rc));
      return rc;
    }

    rc = index->insert_entry(new_record.data(), &new_record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert entry to index. table=%s, index=%s, rid=%s, rc=%s",
               table_meta_->name(), index->index_meta().name(), 
               new_record.rid().to_string().c_str(), strrc(rc));
      return rc;
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
  rc = record_handler_->insert_record(new_record.data(), new_record.len(), const_cast<RID *>(&new_record.rid()));
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to update record. table=%s, rid=%s, rc=%s",
             table_meta_->name(), old_record.rid().to_string().c_str(), strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}
// 清除field and index
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
  else {
    LOG_WARN("record_handler_ is nullptr when close table engine. table=%s", table_meta_->name());
  }

  LOG_INFO("Table has been closed: %s", table_meta_->name());
  return RC::SUCCESS;
}

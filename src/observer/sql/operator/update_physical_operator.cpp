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
// Created by WangYunlai on 2022/6/27.
//

#include "sql/operator/update_physical_operator.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"
#include "storage/field/field.h"
#include "storage/field/field_meta.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const char *attribute_name, Value *value)
    : table_(table), attribute_name_(attribute_name), value_(value)
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  unique_ptr<PhysicalOperator> &child = children_[0];

  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  trx_ = trx;

  // 收集需要更新的记录
  while (OB_SUCC(rc = child->next())) {
    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      LOG_WARN("failed to get current record: %s", strrc(rc));
      return rc;
    }

    RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
    Record &record = row_tuple->record();
    records_.emplace_back(std::move(record));
  }

  child->close();

  // 执行更新操作
  for (Record &record : records_) {
    // 创建新记录
    Record new_record;
    rc = create_updated_record(record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create updated record: %s", strrc(rc));
      return rc;
    }

    // 通过事务更新记录
    rc = trx_->update_record(table_, record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record: %s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::next()
{
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close()
{
  records_.clear();
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::create_updated_record(const Record &old_record, Record &new_record)
{
  // 复制旧记录的数据
  new_record.copy_data(old_record.data(), old_record.len());
  new_record.set_rid(old_record.rid());

  // 获取要更新的字段
  const TableMeta &table_meta = table_->table_meta();
  const FieldMeta *field_meta = table_meta.field(attribute_name_);
  if (nullptr == field_meta) {
    LOG_WARN("field not found. table=%s, field=%s", table_->name(), attribute_name_);
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  // 创建字段对象
  Field field(table_meta, field_meta);
  
  // 设置新值
  RC rc = field.set_value(new_record, *value_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to set field value. table=%s, field=%s, rc=%s", 
             table_->name(), attribute_name_, strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}

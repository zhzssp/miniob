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
#include "sql/expr/tuple.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const char *attribute_name, Value *value)
  : table_(table), attribute_name_(attribute_name), value_(value), trx_(nullptr)
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  trx_ = trx;

  if (children_.empty()) {
    return RC::SUCCESS;
  }

  unique_ptr<PhysicalOperator> &child = children_[0];

  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  // 收集所有需要更新的记录
  while (OB_SUCC(rc = child->next())) {
    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      LOG_WARN("failed to get current tuple: %s", strrc(rc));
      return rc;
    }

    RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
    Record   &record    = row_tuple->record();
    records_.emplace_back(std::move(record));
  }

  child->close();

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 执行更新
  return update_all_records();
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
  const TableMeta &table_meta = table_->table_meta();
  const FieldMeta *field_meta = table_meta.field(attribute_name_);
  if (field_meta == nullptr) {
    LOG_WARN("no such field: %s", attribute_name_);
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  // 复制旧记录内容
  RC rc = new_record.copy_data(old_record.data(), table_meta.record_size());
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to copy old record data: %s", strrc(rc));
    return rc;
  }

  // 写入新值
  Value casted_value;
  if (value_->attr_type() != field_meta->type()) {
    rc = Value::cast_to(*value_, field_meta->type(), casted_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to cast value to field type. field=%s, rc=%s", attribute_name_, strrc(rc));
      return rc;
    }
  } else {
    casted_value = *value_;
  }

  // 将值写入到对应字段偏移
  rc = new_record.set_field(field_meta->offset(), field_meta->len(), (char *)casted_value.data());
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to set field value: %s", strrc(rc));
    return rc;
  }

  // 保留旧记录的RID用于定位更新
  new_record.set_rid(old_record.rid());
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::update_all_records()
{
  RC rc = RC::SUCCESS;
  for (Record &old_record : records_) {
    Record new_record;
    rc = create_updated_record(old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create updated record: %s", strrc(rc));
      return rc;
    }

    rc = trx_->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record: %s", strrc(rc));
      return rc;
    }
  }
  return RC::SUCCESS;
}









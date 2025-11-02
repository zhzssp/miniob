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
// Created by WangYunlai on 2021/6/9.
//

#include "sql/operator/table_scan_physical_operator.h"
#include "event/sql_debug.h"
#include "storage/table/table.h"

using namespace std;

RC TableScanPhysicalOperator::open(Trx *trx)
{
  // record_scanner在此初始化
  RC rc = table_->get_record_scanner(record_scanner_, trx, mode_);
  if (rc == RC::SUCCESS) {
    tuple_.set_schema(table_, table_->table_meta().field_metas());
  }
  trx_ = trx;
  return rc;
}

RC TableScanPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;

  bool filter_result = false;
  if(record_scanner_ == nullptr) {
    LOG_ERROR("In table_scan_physical_operator, record_scanner_ is nullptr");
    return RC::INTERNAL;
  }
  // 这里next出问题 ？--> record在从底下读出来时，是否没有bitmap的信息了？
  while (OB_SUCC(rc = record_scanner_->next(current_record_))) {
    LOG_TRACE("got a record. rid=%s", current_record_.rid().to_string().c_str());

    // 重新获取vector<bool> is_null_信息
    const TableMeta &table_meta = table_->table_meta();
    LOG_INFO("Initialize bitmap of length %d in TableScannerPhysicalOperator::next()", table_meta.field_num());
    current_record_.init_bitmap(table_meta.field_num());

    bool *null_bitmap = reinterpret_cast<bool *>(current_record_.data() + table_meta.fields_record_size());
    for(int i = 0; i < table_meta.bitmap_record_size() / sizeof(bool); i++) {
      if(null_bitmap[i]) {
        current_record_.set_is_null(i);
      }
    }

    // 将从表中读取到的记录, 以元组格式进行保存
    tuple_.set_record(&current_record_);
    rc = filter(tuple_, filter_result);
    if (rc != RC::SUCCESS) {
      LOG_TRACE("record filtered failed=%s", strrc(rc));
      return rc;
    }

    // 结果已被保存到tuple_中，通过current_tuple()即可获取
    if (filter_result) {
      sql_debug("Get a tuple: %s", tuple_.to_string().c_str());
      break;
    } else {
      sql_debug("A tuple is filtered: %s", tuple_.to_string().c_str());
    }
  }
  return rc;
}

RC TableScanPhysicalOperator::close() {
  RC rc = RC::SUCCESS;
  if (record_scanner_ != nullptr) {
    rc = record_scanner_->close_scan();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close record scanner");
    }
    delete record_scanner_;
    record_scanner_ = nullptr;
  }
  return rc;

}

Tuple *TableScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}

string TableScanPhysicalOperator::param() const { return table_->name(); }

void TableScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

/* 返回结果为false时，表示当前tuple被过滤掉 */
RC TableScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  RC    rc = RC::SUCCESS;
  Value value;
  // 每个过滤条件仅针对单个字段 --> 获取对应Value进行判断 ？
  for (unique_ptr<Expression> &expr : predicates_) {
    // 还不是OrderedUnBoundFieldExpr，类型是ComparisonExpr ？
    rc = expr->get_value(tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    bool tmp_result = value.get_boolean();
    if (!tmp_result) {
      result = false;
      return rc;
    }
  }

  result = true;
  return rc;
}

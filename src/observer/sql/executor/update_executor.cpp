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
// Created by Wangyunlai on 2023/6/13.
//

#include "sql/executor/update_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/update_stmt.h"
#include "storage/db/db.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/table/table.h"
#include "storage/record/record_scanner.h"
#include "storage/field/field_meta.h"
#include "storage/trx/trx.h"

RC UpdateExecutor::execute(SQLStageEvent *sql_event)
{
    LOG_TRACE("UpdateExecutor::execute is called");
    Stmt    *stmt    = sql_event->stmt();
    Session *session = sql_event->session_event()->session();
    ASSERT(stmt->type() == StmtType::UPDATE,
        "update executor can not run this command: %d",
        static_cast<int>(stmt->type()));

    UpdateStmt *update_stmt = static_cast<UpdateStmt *>(stmt);

    Table *table = update_stmt->table();
    const char *attribute_name = update_stmt->attribute_name();
    Value *value = update_stmt->value();
    FilterStmt *filter_stmt = update_stmt->filter_stmt();

    RC rc = RC::SUCCESS;
    const TableMeta &table_meta = table->table_meta();
    const FieldMeta *field_meta = table_meta.field(attribute_name);
    if (field_meta == nullptr) {
      LOG_WARN("no such field. table=%s, field=%s", table->name(), attribute_name);
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }

    if(!table_meta.field_nullable(field_meta->field_id()) && value->is_null()) {
      LOG_INFO("Try to set not nullable field to be null, return failure");
      return RC::NOT_NULL;
    }

    // 准备更新为的值
    Value final_value;
    if (!value->is_null() && value->attr_type() != field_meta->type()) {
      rc = Value::cast_to(*value, field_meta->type(), final_value);
      if (rc != RC::SUCCESS) {
        LOG_WARN("type mismatch and cannot cast. field=%s.%s, rc=%s", table->name(), attribute_name, strrc(rc));
        return rc;
      }
    } else {
      LOG_INFO("Value's type is consistent or value is null !");
      final_value = *value;
    }

    // 检查并更新
    RecordScanner *scanner = nullptr;
    rc = table->get_record_scanner(scanner, session->current_trx(), ReadWriteMode::READ_WRITE);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    
    // 将record给从页面中读取上来 --> 此时不需要对null的情况进行排除，都是旧的数据
    vector<Record> targets;
    Record record;
    while (OB_SUCC(rc = scanner->next(record))) {   // 直接使用record_scanner，此时尚没有将record的vector<bool>重构
      bool selected = true;
      if (filter_stmt != nullptr) {
        const auto &units = filter_stmt->filter_units();
        for (const FilterUnit *unit : units) {
          const FilterObj &lobj = unit->left();
          const FilterObj &robj = unit->right();
          
          // 将表达式中的字段转化为相应的值，比如age > 0中age替换为record中的具体值
          Value lval;
          if (lobj.is_attr) {
            const FieldMeta *fm = lobj.field.meta();
            lval.set_type(fm->type());
            lval.set_data(record.data() + fm->offset(), fm->len());
            lval.set_null(record.get_null_information(fm->field_id(), table_meta.fields_record_size()));
            LOG_INFO("field %d, lval's null info is %s", fm->field_id(), record.get_null_information(fm->field_id(), table_meta.fields_record_size()) ? "true" : "false");
          } else {
            lval = lobj.value;
          }

          Value rval;
          if (robj.is_attr) {
            const FieldMeta *fm = robj.field.meta();
            rval.set_type(fm->type());
            rval.set_data(record.data() + fm->offset(), fm->len());
            rval.set_null(record.get_null_information(fm->field_id(), table_meta.fields_record_size()));
            LOG_INFO("field %d, rval's null info is %s", fm->field_id(), record.get_null_information(fm->field_id(), table_meta.fields_record_size()) ? "true" : "false");
          } else {
            rval = robj.value;
          }
          
          // 只有非null的时候进行cast
          if (!lval.is_null() && !rval.is_null() && lval.attr_type() != rval.attr_type()) {
            LOG_WARN("Types not match, left attr_type = %d, right attr_type = %d", lval.attr_type(), rval.attr_type());
            Value casted;
            RC crc = Value::cast_to(rval, lval.attr_type(), casted);
            if (crc == RC::SUCCESS) {
              rval = casted;
            } else {
              crc = Value::cast_to(lval, rval.attr_type(), casted);
              if (crc == RC::SUCCESS) {
                lval = casted;
              } else {
                selected = false;
                break;
              }
            }
          }
          
          // 执行的是where的筛选逻辑 --> 这里如果compare null会出问题
          bool pass = false;
          if(!lval.is_null() && !rval.is_null()) {
            int cmp = lval.compare(rval);

            switch (unit->comp()) {
            case EQUAL_TO: pass = (cmp == 0); break;
            case NOT_EQUAL: pass = (cmp != 0); break;
            case LESS_THAN: pass = (cmp < 0); break;
            case LESS_EQUAL: pass = (cmp <= 0); break;
            case GREAT_THAN: pass = (cmp > 0); break;
            case GREAT_EQUAL: pass = (cmp >= 0); break;
            default: pass = false; break;
          }
          } else {
            LOG_INFO("When trying to update, null appears in filter condition");
            if(unit->comp() == IS_OP && rval.is_null()) {
              LOG_INFO("When updating, filter's operator is IS, lval is %s", lval.to_string().c_str());
              if(lval.is_null()) {
                pass = true;
              } else {
                pass = false;
              }
            } else if(unit->comp() == IS_NOT_OP && rval.is_null()) {
              LOG_INFO("When updating, filter's operator is IS NOT, lval is %s", lval.to_string().c_str());
              if(lval.is_null()) {
                pass = false; 
              } else {
                pass = true;
              }
            } else {
              LOG_ERROR("Cannot find matched numerical operator or operand, pass = false defaultly");
              pass = false;
            }
          }
          // 出现无法满足的条件 --> 直接跳过，查看下一个元组
          LOG_INFO("Pass value is %s", pass ? "true" : "false");
          if (!pass) { selected = false; break; }
        }
      }

      if (selected) {
        targets.emplace_back(std::move(record));
      }
    }
    scanner->close_scan();
    delete scanner;
    if (rc == RC::RECORD_EOF) {
      rc = RC::SUCCESS;
    }
    if (rc != RC::SUCCESS) {
      return rc;
    }

    // 将获取到的record进行更新
    Trx *trx = session->current_trx();
    for (Record &old_record : targets) {
      Record new_record;
      // record的char *data中存储了bitmap --> 这里只将char *data复制过去了 !!!
      rc = new_record.copy_data(old_record.data(), table_meta.record_size());
      if (rc != RC::SUCCESS) {
        return rc;
      }

      // 初始化bitmap --> 需要手动设置
      old_record.init_bitmap(table_meta.field_num());
      new_record.init_bitmap(table_meta.field_num());

      for(int i = 0; i < table_meta.field_num(); i++) {
        bool is_null_info = old_record.get_null_information(i, table_meta.fields_record_size());
        if(is_null_info) {
          old_record.set_is_null(i);
          new_record.set_is_null(i);
        } else {
          old_record.set_is_not_null(i);
          new_record.set_is_not_null(i);
        }
      }
      
      // 只是将要修改的字段的新值给复制过去 --> null对应的这段内存无用，全部置为0
      if(!final_value.is_null()) {
        rc = new_record.set_field(field_meta->offset(), field_meta->len(), (char *)final_value.data());

        // 原本的数据是null --> 修改信息
        if(old_record.is_null(field_meta->field_id())) {
          LOG_INFO("Update field %d's null value to be non-null value", field_meta->field_id());
          new_record.set_bitmap(field_meta->field_id(), table_meta.fields_record_size(), false);
          new_record.set_is_not_null(field_meta->field_id());
        }
      }
      else {
        LOG_INFO("Get null final_value, update field %d's is_null_ information", field_meta->field_id());

        // 清空对应字段原本的值
        vector<char> zero(field_meta->len(), 0);
        rc = new_record.set_field(field_meta->offset(), field_meta->len(), zero.data());

        new_record.set_bitmap(field_meta->field_id(), table_meta.fields_record_size(), true);
        new_record.set_is_null(field_meta->field_id());
      }
      if (rc != RC::SUCCESS) {
        return rc;
      }

      new_record.set_rid(old_record.rid());

      rc = trx->update_record(table, old_record, new_record);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to update record: %s", strrc(rc));
        return rc;
      }
    }

    return RC::SUCCESS;
}



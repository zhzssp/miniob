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
// Created by Assistant on 2024/12/19.
//

#include "sql/expr/subquery_expr.h"
#include <cmath>
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/expr/arithmetic_operator.hpp"
#include "sql/parser/parse_defs.h"
#include "sql/operator/physical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/stmt/select_stmt.h"
using namespace std;


SubqueryExpr::SubqueryExpr(ParsedSqlNode *sub_query_sn) : sub_query_sn_(sub_query_sn) {}

std::unique_ptr<Expression> SubqueryExpr::copy() const {
  auto *copy = new SubqueryExpr(sub_query_sn_);
  // 拷贝时共享 stmt_shared_
  if (stmt_shared_ != nullptr) {
    copy->stmt_shared_ = stmt_shared_;
    // 使用 NoopDeleter，不释放对象，由 shared_ptr 管理
    copy->stmt_ = SelectStmtPtr(stmt_shared_.get());
  }
  return std::unique_ptr<Expression>(copy);
}

RC SubqueryExpr::open_physical_operator(Tuple *outer_tuple) const
{
  if (physical_operator_ == nullptr) {
    LOG_WARN("physical operator is null");
    return RC::INVALID_ARGUMENT;
  }
  // 将外层的 tuple 传递给子查询算子，以达到查外层表的目的
  // proj -> orderby -> predicate 普通
  // proj -> orderby -> groupby -> predicate 聚合
  physical_operator_->set_outer_tuple(outer_tuple);
  RC rc = physical_operator_->open(trx_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open physical operator. rc=%s", strrc(rc));
  } else {
    is_open_ = true;
  }
  return rc;
}
RC SubqueryExpr::close_physical_operator() const
{
  if (physical_operator_ == nullptr) {
    LOG_WARN("physical operator is null");
    return RC::INVALID_ARGUMENT;
  }
  RC rc = physical_operator_->close();
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to close physical operator. rc=%s", strrc(rc));
  } else {
    is_open_ = false;
  }
  return rc;
}

int      SubqueryExpr::value_length() const { return sizeof(int); }
RC       SubqueryExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;
  // 忘记去年为什么这里要加这个判断了，先加上吧
  if (logical_operator_ == nullptr && physical_operator_ == nullptr) {
    return RC::RECORD_EOF;
  }

  if (physical_operator_ == nullptr) {
    LOG_WARN("physical operator is null");
    return RC::INVALID_ARGUMENT;
  }


  auto *tuple__ = const_cast<Tuple*>(&tuple);
  if (!is_open_) {
    rc = open_physical_operator(tuple__);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to open physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  // 开始执行物理操作
  LOG_WARN("SubqueryExpr: before physical_operator_->next()");
  rc = physical_operator_->next();
  LOG_WARN("SubqueryExpr: after physical_operator_->next(), rc=%s", strrc(rc));
  if (rc != RC::SUCCESS) {
    if (rc != RC::RECORD_EOF) {
      close_physical_operator(); // 关闭子查询算子
      LOG_PANIC("failed to get next tuple. rc=%s", strrc(rc));
      return rc;
    }
    // EOF
    rc = close_physical_operator();
    if (rc == RC::SUCCESS) {
      rc = RC::RECORD_EOF;
    } else {
      LOG_PANIC("failed to close physical operator. rc=%s", strrc(rc));
    }
    return rc;
  }
  auto tuple_ = physical_operator_->current_tuple();
  // 子查询的结果中，tuple 只能有一个 cell。这里应该不会出现tuple的cell数大于1的情况（在上层就已经排除了）
  if (tuple_->cell_num() > 1) {
    LOG_WARN("tuple cell count is not 1");
    close_physical_operator(); // 关闭子查询算子
    return RC::INVALID_ARGUMENT;
  }
  // if (tuple_->cell_num() == 0) {
  //   value.set_null(true);
  // } else {
  //   tuple_->cell_at(0, value);
  // }
  if (tuple_->cell_num() == 0) {
    LOG_WARN("A warn from SubqueryExpr: tuple cell count is 0");
    return RC::RECORD_EOF;
  }
  tuple_->cell_at(0, value);
  LOG_WARN("SubqueryExpr: got value, type=%d, num_cells=%d", (int)value.attr_type(), tuple_->cell_num());
  // 读取到一条记录后立即关闭子查询算子，释放可能持有的页读锁/Pin，避免后续写入出现锁冲突
  RC close_rc = close_physical_operator();
  if (close_rc != RC::SUCCESS) {
    LOG_WARN("failed to close subquery physical operator after read. rc=%s", strrc(close_rc));
  }
  return rc;
}


void SubqueryExpr::set_logical_operator(std::unique_ptr<LogicalOperator> logical_operator)
{
  logical_operator_ = std::move(logical_operator);
}
void SubqueryExpr::set_physical_operator(std::unique_ptr<PhysicalOperator> physical_operator)
{
  physical_operator_ = std::move(physical_operator);
}
void                               SubqueryExpr::set_trx(Trx *trx) { trx_ = trx; }
void                               SubqueryExpr::set_stmt(std::unique_ptr<SelectStmt> stmt) { 
  stmt_.reset(stmt.get());
  // 同时设置 shared_ptr
  stmt_shared_ = std::shared_ptr<SelectStmt>(stmt.release());
}
void SubqueryExpr::set_stmt_shared(std::shared_ptr<SelectStmt> stmt) {
  stmt_shared_ = stmt;
  // 如果还没有设置 stmt_，则从 shared_ptr 获取
  if (!stmt_ && stmt_shared_ != nullptr) {
    // 创建一个非拥有的 unique_ptr
    stmt_.reset(stmt_shared_.get());
  }
}
ParsedSqlNode                     *SubqueryExpr::sub_query_sn() { return sub_query_sn_; }
SelectStmt* SubqueryExpr::stmt() { return stmt_.get(); }
std::shared_ptr<SelectStmt> &SubqueryExpr::stmt_shared() { return stmt_shared_; }
std::unique_ptr<LogicalOperator>  &SubqueryExpr::logical_operator() { return logical_operator_; }
std::unique_ptr<PhysicalOperator> &SubqueryExpr::physical_operator() { return physical_operator_; }


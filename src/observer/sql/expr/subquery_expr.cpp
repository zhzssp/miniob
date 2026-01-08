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
#include "sql/stmt/select_stmt.h"
#include "sql/parser/parse_defs.h"
#include "sql/operator/physical_operator.h"
#include "sql/operator/logical_operator.h"
#include "common/log/log.h"
#include "common/lang/memory.h"

using namespace std;

SubqueryExpr::SubqueryExpr(ParsedSqlNode* sub_query_sn)
    : sub_query_sn_(sub_query_sn), stmt_(nullptr), is_open_(false), trx_(nullptr)
{}

unique_ptr<Expression> SubqueryExpr::copy() const
{
  // 复用解析树指针与共享的 stmt_。物理/逻辑算子需由上层重新设置
  auto expr = std::make_unique<SubqueryExpr>(sub_query_sn_);
  if (stmt_shared_) {
    expr->set_stmt_shared(stmt_shared_);
  } else if (stmt_.get() != nullptr) {
    // 不持有所有权，仅共享原始指针
    expr->set_stmt(std::unique_ptr<SelectStmt>(stmt_.get()));
  }
  return expr;
}

RC SubqueryExpr::get_value(const Tuple &tuple, Value &value) const
{
  // 标量子查询：执行子查询并返回第一行第一列的值
  LOG_WARN("SubqueryExpr::get_value: called, physical_operator_=%p, is_open_=%d", 
           physical_operator_.get(), is_open_);
  
  if (!physical_operator_) {
    LOG_WARN("SubqueryExpr::get_value: physical_operator_ is null");
    return RC::INVALID_ARGUMENT;
  }

  // 如果已经打开，先关闭（因为外层元组可能已经改变，需要重新执行）
  if (is_open_) {
    LOG_WARN("SubqueryExpr::get_value: closing already open physical operator");
    RC close_rc = close_physical_operator();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("SubqueryExpr::get_value: failed to close already open physical operator. rc=%s", strrc(close_rc));
    }
  }

  Tuple *outer_tuple_ptr = const_cast<Tuple*>(&tuple);
  RC rc = open_physical_operator(outer_tuple_ptr);
  if (rc != RC::SUCCESS) {
    LOG_WARN("SubqueryExpr::get_value: failed to open physical operator. rc=%s", strrc(rc));
    return rc;
  }

  // 获取第一行
  rc = physical_operator_->next();
  if (rc != RC::SUCCESS) {
    LOG_WARN("SubqueryExpr::get_value: failed to get first row. rc=%s", strrc(rc));
    RC close_rc = close_physical_operator();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("SubqueryExpr::get_value: failed to close physical operator. rc=%s", strrc(close_rc));
    }
    // 如果没有行，返回 NULL 值
    value.set_null(true);
    return RC::SUCCESS;
  }

  // 在获取 tuple 之前，确保 current_tuple() 被调用以设置 child_tuple_
  auto sub_t = physical_operator_->current_tuple();
  if (!sub_t || sub_t->cell_num() == 0) {
    LOG_WARN("SubqueryExpr::get_value: tuple is null or has no cells");
    RC close_rc = close_physical_operator();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("SubqueryExpr::get_value: failed to close physical operator. rc=%s", strrc(close_rc));
    }
    value.set_null(true);
    return RC::SUCCESS;
  }

  LOG_WARN("SubqueryExpr::get_value: got tuple with %d cells", sub_t->cell_num());
  
  // 获取第一列的值
  rc = sub_t->cell_at(0, value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("SubqueryExpr::get_value: failed to get first cell. rc=%s, tuple=%p, cell_num=%d", 
             strrc(rc), sub_t, sub_t ? sub_t->cell_num() : -1);
    RC close_rc = close_physical_operator();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("SubqueryExpr::get_value: failed to close physical operator. rc=%s", strrc(close_rc));
    }
    return rc;
  }
  
  LOG_WARN("SubqueryExpr::get_value: successfully got value=%s", value.to_string().c_str());

  // 检查是否有多行（标量子查询应该只返回一行）
  RC next_rc = physical_operator_->next();
  if (next_rc == RC::SUCCESS) {
    LOG_WARN("SubqueryExpr::get_value: subquery returned more than one row, scalar subquery must return at most one row");
    RC close_rc = close_physical_operator();
    if (close_rc != RC::SUCCESS) {
      LOG_WARN("SubqueryExpr::get_value: failed to close physical operator. rc=%s", strrc(close_rc));
    }
    return RC::SUBQUERY_MULTIPLE_ROWS;
  }

  // 关闭物理算子（注意：不要在这里关闭，因为可能会影响后续的调用）
  // 实际上，我们应该在每次调用时都重新打开和关闭，所以这里关闭是安全的
  RC close_rc = close_physical_operator();
  if (close_rc != RC::SUCCESS) {
    LOG_WARN("SubqueryExpr::get_value: failed to close physical operator. rc=%s", strrc(close_rc));
  }

  return RC::SUCCESS;
}

int SubqueryExpr::value_length() const
{
  return -1;
}

void SubqueryExpr::set_logical_operator(std::unique_ptr<LogicalOperator> logical_operator)
{
  logical_operator_ = std::move(logical_operator);
}

void SubqueryExpr::set_physical_operator(std::unique_ptr<PhysicalOperator> physical_operator)
{
  physical_operator_ = std::move(physical_operator);
}

void SubqueryExpr::set_trx(Trx *trx)
{
  trx_ = trx;
}

RC SubqueryExpr::open_physical_operator(Tuple *outer_tuple) const
{
  if (!physical_operator_) {
    return RC::INVALID_ARGUMENT;
  }
  physical_operator_->set_outer_tuple(outer_tuple);
  RC rc = physical_operator_->open(trx_);
  if (rc == RC::SUCCESS) {
    is_open_ = true;
  }
  return rc;
}

RC SubqueryExpr::close_physical_operator() const
{
  if (!physical_operator_) {
    return RC::SUCCESS;
  }
  if (is_open_) {
    RC rc = physical_operator_->close();
    is_open_ = false;
    return rc;
  }
  return RC::SUCCESS;
}

void SubqueryExpr::set_stmt(std::unique_ptr<SelectStmt> stmt)
{
  // 仅接管原始指针，生命周期可能在别处管理
  SelectStmt *raw = stmt.release();
  stmt_.reset(raw);
}

void SubqueryExpr::set_stmt_shared(std::shared_ptr<SelectStmt> stmt)
{
  stmt_shared_ = std::move(stmt);
  // 确保 stmt_ 的原始指针与 shared 保持一致，避免空悬
  stmt_.reset(stmt_shared_.get());
}

ParsedSqlNode* SubqueryExpr::sub_query_sn()
{
  return sub_query_sn_;
}

SelectStmt* SubqueryExpr::stmt()
{
  if (stmt_shared_) return stmt_shared_.get();
  return stmt_.get();
}

std::shared_ptr<SelectStmt> &SubqueryExpr::stmt_shared()
{
  return stmt_shared_;
}

std::unique_ptr<LogicalOperator> &SubqueryExpr::logical_operator()
{
  return logical_operator_;
}

std::unique_ptr<PhysicalOperator> &SubqueryExpr::physical_operator()
{
  return physical_operator_;
}

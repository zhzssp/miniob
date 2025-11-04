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
  // 子查询通常不直接作为标量值返回，这里保持未实现
  return RC::UNIMPLEMENTED;
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

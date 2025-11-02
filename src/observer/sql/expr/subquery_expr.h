#pragma once

#include <memory>
#include <string>
#include <functional>

#include "common/value.h"
#include "storage/field/field.h"
#include "sql/expr/aggregator.h"
#include "storage/common/chunk.h"
#include "sql/expr/expression.h"

class Tuple;
class ParsedSqlNode;
class SelectStmt;
class LogicalOperator;
class PhysicalOperator;

// 自定义 deleter，不释放对象
struct NoopDeleter {
  void operator()(SelectStmt*) const {}
};

using SelectStmtPtr = std::unique_ptr<SelectStmt, NoopDeleter>;

class SubqueryExpr : public Expression
{
public:
  SubqueryExpr(ParsedSqlNode* sub_query_sn);
  ExprType type() const override { return ExprType::SUB_QUERY; }
  AttrType value_type() const override { return AttrType::UNDEFINED; }
  int      value_length() const override;
  RC       get_value(const Tuple &tuple, Value &value) const override;

  // 提供深拷贝：复用解析树指针，共享 stmt_shared_
  // 注意：拷贝后的 SubqueryExpr 需要重新设置 logical_operator_, physical_operator_
  std::unique_ptr<Expression> copy() const override;

  void set_logical_operator(std::unique_ptr<LogicalOperator> logical_operator);
  void set_physical_operator(std::unique_ptr<PhysicalOperator> physical_operator);
  void set_trx(Trx *trx);
  RC   open_physical_operator(Tuple *outer_tuple) const;
  RC   close_physical_operator() const;
  void set_stmt(std::unique_ptr<SelectStmt> stmt);
  void set_stmt_shared(std::shared_ptr<SelectStmt> stmt);  // 新增方法用于设置 shared_ptr
  ParsedSqlNode* sub_query_sn();
  SelectStmt* stmt();  // 返回原始指针而不是 unique_ptr&
  std::shared_ptr<SelectStmt> &stmt_shared();  // 新增方法用于获取 shared_ptr
  std::unique_ptr<LogicalOperator> &logical_operator();
  std::unique_ptr<PhysicalOperator> &physical_operator();
  
private:
  ParsedSqlNode* sub_query_sn_;
  SelectStmtPtr    stmt_;  // 使用自定义 deleter 的 unique_ptr
  std::shared_ptr<SelectStmt>    stmt_shared_;  // 新增用于共享
  std::unique_ptr<LogicalOperator> logical_operator_;
  std::unique_ptr<PhysicalOperator> physical_operator_;
  mutable bool is_open_ = false;
  mutable Trx *trx_;
};
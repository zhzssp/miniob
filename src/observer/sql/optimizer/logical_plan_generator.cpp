/* Copyright (c) 2023 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/08/16.
//

#include "sql/optimizer/logical_plan_generator.h"

#include "common/log/log.h"

#include "sql/operator/calc_logical_operator.h"
#include "sql/operator/delete_logical_operator.h"
#include "sql/operator/explain_logical_operator.h"
#include "sql/operator/insert_logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/project_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/group_by_logical_operator.h"
#include "sql/operator/order_by_logical_operator.h"

#include "sql/expr/expression.h"
#include "sql/expr/subquery_expr.h"

#include "sql/stmt/calc_stmt.h"
#include "sql/stmt/delete_stmt.h"
#include "sql/stmt/explain_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/stmt/insert_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"

#include "sql/expr/expression_iterator.h"

using namespace std;
using namespace common;

RC LogicalPlanGenerator::create(Stmt *stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  RC rc = RC::SUCCESS;
  switch (stmt->type()) {
    case StmtType::CALC: {
      CalcStmt *calc_stmt = static_cast<CalcStmt *>(stmt);

      rc = create_plan(calc_stmt, logical_operator);
    } break;

    case StmtType::SELECT: {
      SelectStmt *select_stmt = static_cast<SelectStmt *>(stmt);

      rc = create_plan(select_stmt, logical_operator);
    } break;

    case StmtType::INSERT: {
      InsertStmt *insert_stmt = static_cast<InsertStmt *>(stmt);

      rc = create_plan(insert_stmt, logical_operator);
    } break;

    case StmtType::DELETE: {
      DeleteStmt *delete_stmt = static_cast<DeleteStmt *>(stmt);

      rc = create_plan(delete_stmt, logical_operator);
    } break;

    case StmtType::EXPLAIN: {
      ExplainStmt *explain_stmt = static_cast<ExplainStmt *>(stmt);

      rc = create_plan(explain_stmt, logical_operator);
    } break;
    default: {
      rc = RC::UNIMPLEMENTED;
    }
  }
  return rc;
}

RC LogicalPlanGenerator::create_plan(CalcStmt *calc_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  logical_operator.reset(new CalcLogicalOperator(std::move(calc_stmt->expressions())));
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  LOG_TRACE("Begin to create a plan for basic select");
  unique_ptr<LogicalOperator> *last_oper = nullptr;

  unique_ptr<LogicalOperator> table_oper(nullptr);
  // 最下层
  LOG_TRACE("Table is the first level logical operator");
  last_oper = &table_oper;
  unique_ptr<LogicalOperator> predicate_oper;

  RC rc = create_plan(select_stmt->filter_stmt(), predicate_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create predicate logical plan. rc=%s", strrc(rc));
    return rc;
  }

  const vector<Table *> &tables = select_stmt->tables();
  
  for (size_t i = 0; i < tables.size(); i++) {
    Table *table = tables[i];
    // 这里设置READ_ONLY --> 
    unique_ptr<LogicalOperator> table_get_oper(new TableGetLogicalOperator(table, ReadWriteMode::READ_ONLY));
    
    // 只在单表查询
    if (table_oper == nullptr) {
      LOG_TRACE("Move table_get_oper to table_oper directly");
      table_oper = std::move(table_get_oper);
    } else {
      // 什么时候table_oper不是nullptr ??? --> 存在多表联查
      JoinLogicalOperator *join_oper = new JoinLogicalOperator;
      LOG_TRACE("Join logical operator adds table %d's logical operator and its table_get logical operator to child_", i);
      join_oper->add_child(std::move(table_oper));
      join_oper->add_child(std::move(table_get_oper));
      
      // 精确条件分配：只处理与当前 JOIN 相关的条件
      if (select_stmt->join_filter_stmt() != nullptr) {
        const auto &filter_units = select_stmt->join_filter_stmt()->filter_units();
        
        for (const auto &filter_unit : filter_units) {
          // 检查条件是否与当前 JOIN 相关
          bool is_relevant = false;
          
          // 获取当前 JOIN 涉及的表
          Table *left_table = nullptr;
          Table *right_table = table; // 右表是当前表
          
          
          // 左子树：可能是单个表或之前 JOIN 的结果
          if (table_oper != nullptr && table_oper->type() == LogicalOperatorType::TABLE_GET) {
            auto *left_table_get = dynamic_cast<TableGetLogicalOperator*>(table_oper.get());
            if (left_table_get != nullptr) {
              left_table = left_table_get->table();
            }
          } else if (table_oper != nullptr && table_oper->type() == LogicalOperatorType::JOIN) {
            // 对于 JOIN 类型的左子树，我们无法直接确定左表
            // 但我们可以通过检查条件中的字段来确定
            // 暂时跳过左表检查，让条件匹配逻辑自己处理
          }
          
          // 检查条件是否涉及当前 JOIN 的表
          if (filter_unit->left().is_attr && filter_unit->right().is_attr) {
            const Table *left_field_table = filter_unit->left().field.table();
            const Table *right_field_table = filter_unit->right().field.table();
            
           // LOG_WARN("JOIN: Checking condition %s.%s = %s.%s", 
                     left_field_table ? left_field_table->name() : "NULL",
                     filter_unit->left().field.field_name(),
                     right_field_table ? right_field_table->name() : "NULL",
                     filter_unit->right().field.field_name();
            
            
            // 条件涉及左表和右表
            if (left_table != nullptr) {
              // 左表已知，检查条件是否涉及左表和右表
              if ((left_field_table == left_table && right_field_table == right_table) ||
                  (left_field_table == right_table && right_field_table == left_table)) {
                is_relevant = true;
              } else {
              }
            } else {
              // 左表未知（可能是 JOIN 结果），检查条件是否涉及右表
              if (left_field_table == right_table || right_field_table == right_table) {
                is_relevant = true;
              } else {
              }
            }
          }
          
          if (is_relevant) {
            // 将 FilterUnit 转换为 Expression
            auto left_expr = LogicalPlanGenerator::create_expression_from_filter_obj(filter_unit->left());
            auto right_expr = LogicalPlanGenerator::create_expression_from_filter_obj(filter_unit->right());
            if (left_expr != nullptr && right_expr != nullptr) {
              auto comp_expr = make_unique<ComparisonExpr>(filter_unit->comp(), std::move(left_expr), std::move(right_expr));
              
              // 只有等值条件用于 JOIN，非等值条件用于后续过滤
              if (filter_unit->comp() == CompOp::EQUAL_TO) {
                join_oper->add_join_predicate(std::move(comp_expr));
              } else {
                // 非等值条件添加到 predicate_oper 中
                if (predicate_oper == nullptr) {
                  predicate_oper = make_unique<PredicateLogicalOperator>(std::move(comp_expr));
                } else {
                  // 如果已经有 predicate_oper，需要创建 ConjunctionExpr 来组合条件
                  auto existing_expr = std::move(predicate_oper->expressions()[0]);
                  predicate_oper->expressions().clear();
                  
                  vector<unique_ptr<Expression>> conjunction_children;
                  conjunction_children.push_back(std::move(existing_expr));
                  conjunction_children.push_back(std::move(comp_expr));
                  
                  auto conjunction_expr = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, conjunction_children);
                  predicate_oper = make_unique<PredicateLogicalOperator>(std::move(conjunction_expr));
                }
              }
            }
          }
        }
      }
      
      // 第二层
      LOG_TRACE("Set join %d as the second level logical operator", i);
      table_oper = unique_ptr<LogicalOperator>(join_oper);
      
      
      // 如果有过滤条件，将其设置到 JoinLogicalOperator 中
      if (predicate_oper) {
        join_oper->add_predicate_op(predicate_oper.get());
      }
    }
  }


  // 第三层
  if (predicate_oper) {
    LOG_TRACE("Set predicate(filter) as the third level logical operator");
    if (*last_oper) {
      predicate_oper->add_child(std::move(*last_oper));
    }

    last_oper = &predicate_oper;
  }

  // 第三层
  unique_ptr<LogicalOperator> group_by_oper;
  rc = create_group_by_plan(select_stmt, group_by_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create group by logical plan. rc=%s", strrc(rc));
    return rc;
  }

  if (group_by_oper) {
    LOG_TRACE("Set group by as the forth level logical operator");
    if (*last_oper) {
      group_by_oper->add_child(std::move(*last_oper));
    }

    last_oper = &group_by_oper;
  }

  // 最上层
  unique_ptr<LogicalOperator> order_by_oper;
  rc = create_order_by_plan(select_stmt, order_by_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create order by logical plan. rc=%s", strrc(rc));
    return rc;
  }

  if (order_by_oper) {
    LOG_TRACE("Set order by as the fifth level logical operator");
    if (*last_oper) {
      order_by_oper->add_child(std::move(*last_oper));
    }

    last_oper = &order_by_oper;
  }

  unique_ptr<LogicalOperator> project_oper = make_unique<ProjectLogicalOperator>(std::move(select_stmt->query_expressions()));
  if (*last_oper) {
    // 最后的根节点
    LOG_TRACE("Project logical operator is the root of logical operator tree");
    project_oper->add_child(std::move(*last_oper));
  }

  last_oper = &project_oper;

  logical_operator = std::move(*last_oper);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(FilterStmt *filter_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  LOG_TRACE("Begin to create filter logical operator");
  RC                                  rc = RC::SUCCESS;
  vector<unique_ptr<Expression>> cmp_exprs;
  const vector<FilterUnit *>    &filter_units = filter_stmt->filter_units();
  
  for (const FilterUnit *filter_unit : filter_units) {
    const FilterObj &filter_obj_left  = filter_unit->left();
    const FilterObj &filter_obj_right = filter_unit->right();

    unique_ptr<Expression> left;
    if (filter_obj_left.is_expr) {
      left = unique_ptr<Expression>(filter_obj_left.expression->copy().release());
      // 递归处理表达式中可能包含的子查询
      rc = process_subquery_in_expression(left);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to process subquery in left expression. rc=%s", strrc(rc));
        return rc;
      }
    } else if (filter_obj_left.is_attr) {
      left = make_unique<FieldExpr>(filter_obj_left.field);
    } else {
      left = make_unique<ValueExpr>(filter_obj_left.value);
    }

    unique_ptr<Expression> right;
    LOG_WARN("Creating right expression from FilterObj, is_expr=%d, is_attr=%d", filter_obj_right.is_expr, filter_obj_right.is_attr);
    if (filter_obj_right.is_expr) {
      LOG_WARN("Creating right expression from FilterObj, original expression type=%d", (int)filter_obj_right.expression->type());
      right = unique_ptr<Expression>(filter_obj_right.expression->copy().release());
      LOG_WARN("Right expression after copy, type=%d", (int)right->type());
      // 递归处理表达式中可能包含的子查询
      rc = process_subquery_in_expression(right);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to process subquery in right expression. rc=%s", strrc(rc));
        return rc;
      }
      LOG_WARN("Right expression after process_subquery, type=%d", (int)right->type());
    } else if (filter_obj_right.is_attr) {
      right = make_unique<FieldExpr>(filter_obj_right.field);
    } else {
      right = make_unique<ValueExpr>(filter_obj_right.value);
    }
    
    // 跳过子查询表达式的类型检查
    // 如果 right 的 value_type 是 UNDEFINED，很可能包含子查询
    bool has_subquery = (right->value_type() == AttrType::UNDEFINED) || 
                        (left->value_type() == AttrType::UNDEFINED) ||
                        expression_has_subquery(left) || 
                        expression_has_subquery(right);
    
    LOG_WARN("Checking subquery: has_subquery=%d, left_type=%d, right_type=%d, left_value_type=%d, right_value_type=%d",
             has_subquery, (int)left->type(), (int)right->type(), (int)left->value_type(), (int)right->value_type());
    
    if (has_subquery) {
      LOG_WARN("Found subquery in filter, skipping type check. left_type=%d, right_type=%d", 
                (int)left->type(), (int)right->type());
    }
    
    if (!has_subquery && left->value_type() != right->value_type()) {
      auto left_to_right_cost = implicit_cast_cost(left->value_type(), right->value_type());
      auto right_to_left_cost = implicit_cast_cost(right->value_type(), left->value_type());
      if (left_to_right_cost <= right_to_left_cost && left_to_right_cost != INT32_MAX) {
        ExprType left_type = left->type();
        auto cast_expr = make_unique<CastExpr>(std::move(left), right->value_type());
        if (left_type == ExprType::VALUE) {
          Value left_val;
          if (OB_FAIL(rc = cast_expr->try_get_value(left_val)))
          {
            LOG_WARN("failed to get value from left child", strrc(rc));
            return rc;
          }
          left = make_unique<ValueExpr>(left_val);
        } else {
          left = std::move(cast_expr);
        }
      } else if (right_to_left_cost < left_to_right_cost && right_to_left_cost != INT32_MAX) {
        ExprType right_type = right->type();
        auto cast_expr = make_unique<CastExpr>(std::move(right), left->value_type());
        if (right_type == ExprType::VALUE) {
          Value right_val;
          if (OB_FAIL(rc = cast_expr->try_get_value(right_val)))
          {
            LOG_WARN("failed to get value from right child", strrc(rc));
            return rc;
          }
          right = make_unique<ValueExpr>(right_val);
        } else {
          right = std::move(cast_expr);
        }

      } else {
        rc = RC::UNSUPPORTED;
        LOG_WARN("unsupported cast from %s to %s", attr_type_to_string(left->value_type()), attr_type_to_string(right->value_type()));
        return rc;
      }
    }

    ComparisonExpr *cmp_expr = new ComparisonExpr(filter_unit->comp(), std::move(left), std::move(right));
    cmp_exprs.emplace_back(cmp_expr);
  }

  unique_ptr<PredicateLogicalOperator> predicate_oper;
  if (!cmp_exprs.empty()) {
    unique_ptr<ConjunctionExpr> conjunction_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, cmp_exprs));
    predicate_oper = unique_ptr<PredicateLogicalOperator>(new PredicateLogicalOperator(std::move(conjunction_expr)));
  }

  logical_operator = std::move(predicate_oper);
  return rc;
}

int LogicalPlanGenerator::implicit_cast_cost(AttrType from, AttrType to)
{
  if (from == to) {
    return 0;
  }
  return DataType::type_instance(from)->cast_cost(to);
}

RC LogicalPlanGenerator::process_subquery_in_expression(unique_ptr<Expression> &expr)
{
  if (expr == nullptr) {
    return RC::SUCCESS;
  }

  // 检查当前表达式是否是子查询
  if (expr->type() == ExprType::SUB_QUERY) {
    auto sub_query_expr = static_cast<SubqueryExpr *>(expr.get());
    auto sub_query_stmt = sub_query_expr->stmt();
    if (sub_query_stmt == nullptr) {
      LOG_WARN("subquery statement is null, attempting to re-create from sub_query_sn_");
      // 如果 stmt_ 为 nullptr，尝试从 sub_query_sn_->selection 重新创建
      // 但这需要 db 参数，我们暂时不在这里处理
      LOG_WARN("Cannot re-create subquery stmt without db context");
      return RC::INVALID_ARGUMENT;
    }
    
    unique_ptr<LogicalOperator> sub_query_oper;
    RC rc = create_plan(sub_query_stmt, sub_query_oper);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create subquery logical operator. rc=%s", strrc(rc));
      return rc;
    }
    sub_query_expr->set_logical_operator(std::move(sub_query_oper));
    return RC::SUCCESS;
  }

  // 递归处理子表达式
  switch (expr->type()) {
    case ExprType::COMPARISON: {
      auto cmp_expr = static_cast<ComparisonExpr *>(expr.get());
      // 处理左表达式
      if (cmp_expr->left() != nullptr) {
        RC rc = process_subquery_in_expression(cmp_expr->left());
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
      // 处理右表达式
      if (cmp_expr->right() != nullptr) {
        RC rc = process_subquery_in_expression(cmp_expr->right());
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
    } break;
    
    case ExprType::CONJUNCTION: {
      auto conj_expr = static_cast<ConjunctionExpr *>(expr.get());
      for (auto &child : conj_expr->children()) {
        RC rc = process_subquery_in_expression(child);
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
    } break;
    
    case ExprType::ARITHMETIC: {
      auto arith_expr = static_cast<ArithmeticExpr *>(expr.get());
      if (arith_expr->left() != nullptr) {
        RC rc = process_subquery_in_expression(arith_expr->left());
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
      if (arith_expr->right() != nullptr) {
        RC rc = process_subquery_in_expression(arith_expr->right());
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
    } break;
    
    case ExprType::AGGREGATION: {
      auto agg_expr = static_cast<AggregateExpr *>(expr.get());
      if (agg_expr->child() != nullptr) {
        RC rc = process_subquery_in_expression(agg_expr->child());
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
    } break;
    
    case ExprType::CAST: {
      auto cast_expr = static_cast<CastExpr *>(expr.get());
      if (cast_expr->child() != nullptr) {
        RC rc = process_subquery_in_expression(cast_expr->child());
        if (rc != RC::SUCCESS) {
          return rc;
        }
      }
    } break;
    
    default:
      // 其他类型的表达式不包含子表达式，或不需要特殊处理
      break;
  }

  return RC::SUCCESS;
}

bool LogicalPlanGenerator::expression_has_subquery(unique_ptr<Expression> &expr)
{
  if (expr == nullptr) {
    return false;
  }

  ExprType expr_type = expr->type();
  LOG_WARN("expression_has_subquery: expr_type=%d", (int)expr_type);

  // 检查当前表达式是否是子查询
  if (expr_type == ExprType::SUB_QUERY) {
    LOG_WARN("Found SUB_QUERY expression in expression_has_subquery");
    return true;
  }

  // 递归检查子表达式
  switch (expr_type) {
    case ExprType::COMPARISON: {
      LOG_WARN("Checking COMPARISON expression");
      auto cmp_expr = static_cast<ComparisonExpr *>(expr.get());
      if (cmp_expr->left() != nullptr) {
        LOG_WARN("Checking left child of COMPARISON");
        if (expression_has_subquery(cmp_expr->left())) {
          return true;
        }
      }
      if (cmp_expr->right() != nullptr) {
        LOG_WARN("Checking right child of COMPARISON");
        if (expression_has_subquery(cmp_expr->right())) {
          return true;
        }
      }
    } break;
    
    case ExprType::CONJUNCTION: {
      auto conj_expr = static_cast<ConjunctionExpr *>(expr.get());
      for (auto &child : conj_expr->children()) {
        if (expression_has_subquery(child)) {
          return true;
        }
      }
    } break;
    
    case ExprType::ARITHMETIC: {
      auto arith_expr = static_cast<ArithmeticExpr *>(expr.get());
      if (arith_expr->left() != nullptr && expression_has_subquery(arith_expr->left())) {
        return true;
      }
      if (arith_expr->right() != nullptr && expression_has_subquery(arith_expr->right())) {
        return true;
      }
    } break;
    
    case ExprType::AGGREGATION: {
      auto agg_expr = static_cast<AggregateExpr *>(expr.get());
      if (agg_expr->child() != nullptr && expression_has_subquery(agg_expr->child())) {
        return true;
      }
    } break;
    
    case ExprType::CAST: {
      auto cast_expr = static_cast<CastExpr *>(expr.get());
      if (cast_expr->child() != nullptr && expression_has_subquery(cast_expr->child())) {
        return true;
      }
    } break;
    
    default:
      // 其他类型的表达式不包含子表达式
      break;
  }

  return false;
}

RC LogicalPlanGenerator::create_plan(InsertStmt *insert_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  LOG_INFO("Create a logical plan for InsertStmt");
  Table        *table = insert_stmt->table();
  vector<Value> values(insert_stmt->values(), insert_stmt->values() + insert_stmt->value_amount());

  LOG_TRACE("Initializing insert logical operator");
  InsertLogicalOperator *insert_operator = new InsertLogicalOperator(table, values);
  logical_operator.reset(insert_operator);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(DeleteStmt *delete_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  Table                      *table       = delete_stmt->table();
  FilterStmt                 *filter_stmt = delete_stmt->filter_stmt();   // where过滤器 --> 包含所有的filter_units_，可以表达所有的条件
  unique_ptr<LogicalOperator> table_get_oper(new TableGetLogicalOperator(table, ReadWriteMode::READ_WRITE));

  unique_ptr<LogicalOperator> predicate_oper;

  RC rc = create_plan(filter_stmt, predicate_oper);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  unique_ptr<LogicalOperator> delete_oper(new DeleteLogicalOperator(table));

  if (predicate_oper) {
    predicate_oper->add_child(std::move(table_get_oper));
    delete_oper->add_child(std::move(predicate_oper));
  } else {
    delete_oper->add_child(std::move(table_get_oper));
  }

  logical_operator = std::move(delete_oper);
  return rc;
}

RC LogicalPlanGenerator::create_plan(ExplainStmt *explain_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  unique_ptr<LogicalOperator> child_oper;

  Stmt *child_stmt = explain_stmt->child();

  RC rc = create(child_stmt, child_oper);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create explain's child operator. rc=%s", strrc(rc));
    return rc;
  }

  logical_operator = unique_ptr<LogicalOperator>(new ExplainLogicalOperator);
  logical_operator->add_child(std::move(child_oper));
  return rc;
}

RC LogicalPlanGenerator::create_group_by_plan(SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  LOG_TRACE("Begin to create group by logical operator");
  // 分组的元素列表
  vector<unique_ptr<Expression>> &group_by_expressions = select_stmt->group_by();
  vector<Expression *> aggregate_expressions;
  // 查询的字段列表
  vector<unique_ptr<Expression>> &query_expressions = select_stmt->query_expressions();
  function<RC(unique_ptr<Expression>&)> collector = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      // 表达式在下层算子中返回的chunk的位置
      expr->set_pos(aggregate_expressions.size() + group_by_expressions.size());
      aggregate_expressions.push_back(expr.get());
    }
    rc = ExpressionIterator::iterate_child_expr(*expr, collector);
    return rc;
  };

  // lambda函数
  function<RC(unique_ptr<Expression>&)> bind_group_by_expr = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    for (size_t i = 0; i < group_by_expressions.size(); i++) {
      auto &group_by = group_by_expressions[i];
      if (expr->type() == ExprType::AGGREGATION) {
        break;
      } else if (expr->equal(*group_by)) {
        expr->set_pos(i);
        continue;
      } else {
        rc = ExpressionIterator::iterate_child_expr(*expr, bind_group_by_expr);
      }
    }
    return rc;
  };

 bool found_unbound_column = false;
  function<RC(unique_ptr<Expression>&)> find_unbound_column = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      // do nothing
    } else if (expr->pos() != -1) {
      // do nothing
    } else if (expr->type() == ExprType::FIELD) {
      found_unbound_column = true;
    }else {
      rc = ExpressionIterator::iterate_child_expr(*expr, find_unbound_column);
    }
    return rc;
  };
  

  for (unique_ptr<Expression> &expression : query_expressions) {
    bind_group_by_expr(expression);
  }

  for (unique_ptr<Expression> &expression : query_expressions) {
    find_unbound_column(expression);
  }

  // collect all aggregate expressions
  for (unique_ptr<Expression> &expression : query_expressions) {
    collector(expression);
  }

  if (group_by_expressions.empty() && aggregate_expressions.empty()) {
    // 既没有group by也没有聚合函数，不需要group by
    return RC::SUCCESS;
  }

  if (found_unbound_column) {
    LOG_WARN("column must appear in the GROUP BY clause or must be part of an aggregate function");
    return RC::INVALID_ARGUMENT;
  }

  // 如果只需要聚合，但是没有group by 语句，需要生成一个空的group by 语句

  auto group_by_oper = make_unique<GroupByLogicalOperator>(std::move(group_by_expressions),
                                                           std::move(aggregate_expressions));
  logical_operator = std::move(group_by_oper);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_order_by_plan(SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator) 
{
  LOG_TRACE("Begin to create order by logical operator");
  RC rc = RC::SUCCESS;

  // 检查是否存在 ORDER BY 子句
  vector<unique_ptr<OrderedUnboundFieldExpr>> &order_by_expressions = select_stmt->order_by();
  if (order_by_expressions.empty()) {
    return rc;  // 无 ORDER BY，直接返回
  } else {
    for(auto &expr: order_by_expressions) {
      if(expr == nullptr) {
        LOG_ERROR("Get null order_by_expression when initializing order by logical operator");
        return RC::INVALID_ARGUMENT;
      }
    }
  }

  // 构造 ORDER BY LogicalOperator
  unique_ptr<OrderByLogicalOperator> order_by_logical_op = std::make_unique<OrderByLogicalOperator>(std::move(order_by_expressions));
  if(order_by_logical_op == nullptr) {
    LOG_ERROR("Construct order by logical operator fails, get nullptr !!!");
    return RC::EMPTY;
  }

  // 将左值转化为右值，进行资源转移 --> 不需要delete
  logical_operator = std::move(order_by_logical_op);

  return rc;
}

unique_ptr<Expression> LogicalPlanGenerator::create_expression_from_filter_obj(const FilterObj &filter_obj)
{
  if (filter_obj.is_attr) {
    return make_unique<FieldExpr>(filter_obj.field);
  } else {
    return make_unique<ValueExpr>(filter_obj.value);
  }
}

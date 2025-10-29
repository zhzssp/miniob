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
// Created by Wangyunlai on 2022/12/14.
//

#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/expr/subquery_expr.h"
#include "session/session.h"
#include <unordered_map>
#include "sql/operator/aggregate_vec_physical_operator.h"
#include "sql/operator/calc_logical_operator.h"
#include "sql/operator/calc_physical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/delete_logical_operator.h"
#include "sql/operator/delete_physical_operator.h"
#include "sql/operator/explain_logical_operator.h"
#include "sql/operator/explain_physical_operator.h"
#include "sql/operator/expr_vec_physical_operator.h"
#include "sql/operator/group_by_vec_physical_operator.h"
#include "sql/operator/hash_join_physical_operator.h"
#include "sql/operator/index_scan_physical_operator.h"
#include "sql/operator/insert_logical_operator.h"
#include "sql/operator/insert_physical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/nested_loop_join_physical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/predicate_physical_operator.h"
#include "sql/operator/project_logical_operator.h"
#include "sql/operator/project_physical_operator.h"
#include "sql/operator/project_vec_physical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/table_scan_physical_operator.h"
#include "sql/operator/group_by_logical_operator.h"
#include "sql/operator/group_by_physical_operator.h"
#include "sql/operator/hash_group_by_physical_operator.h"
#include "sql/operator/scalar_group_by_physical_operator.h"
#include "sql/operator/table_scan_vec_physical_operator.h"
#include "sql/operator/update_logical_operator.h"
#include "sql/operator/update_physical_operator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/operator/hash_join_physical_operator.h"

using namespace std;

RC PhysicalPlanGenerator::create(LogicalOperator &logical_operator, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  switch (logical_operator.type()) {
    case LogicalOperatorType::CALC: {
      return create_plan(static_cast<CalcLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::TABLE_GET: {
      return create_plan(static_cast<TableGetLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::PREDICATE: {
      return create_plan(static_cast<PredicateLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::PROJECTION: {
      return create_plan(static_cast<ProjectLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::INSERT: {
      return create_plan(static_cast<InsertLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::DELETE: {
      return create_plan(static_cast<DeleteLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::UPDATE: {
      return create_plan(static_cast<UpdateLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::EXPLAIN: {
      return create_plan(static_cast<ExplainLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::JOIN: {
      return create_plan(static_cast<JoinLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::GROUP_BY: {
      return create_plan(static_cast<GroupByLogicalOperator &>(logical_operator), oper, session);
    } break;

    default: {
      ASSERT(false, "unknown logical operator type");
      return RC::INVALID_ARGUMENT;
    }
  }
  return rc;
}

RC PhysicalPlanGenerator::create_vec(LogicalOperator &logical_operator, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  switch (logical_operator.type()) {
    case LogicalOperatorType::TABLE_GET: {
      return create_vec_plan(static_cast<TableGetLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::PROJECTION: {
      return create_vec_plan(static_cast<ProjectLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::GROUP_BY: {
      return create_vec_plan(static_cast<GroupByLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::EXPLAIN: {
      return create_vec_plan(static_cast<ExplainLogicalOperator &>(logical_operator), oper, session);
    } break;
    default: {
      LOG_WARN("unknown logical operator type: %d", logical_operator.type());
      return RC::INVALID_ARGUMENT;
    }
  }
  return rc;
}

RC PhysicalPlanGenerator::create_plan(TableGetLogicalOperator &table_get_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<Expression>> &predicates = table_get_oper.predicates();
  // 看看是否有可以用于索引查找的表达式
  Table *table = table_get_oper.table();

  Index     *index      = nullptr;
  ValueExpr *value_expr = nullptr;
  bool       has_not_equal = false;  // 检查是否有NOT_EQUAL查询
  
  for (auto &expr : predicates) {
    
    if (expr->type() == ExprType::COMPARISON) {
      auto comparison_expr = static_cast<ComparisonExpr *>(expr.get());
      // 检查是否有NOT_EQUAL查询
      // 创建子查询
      if (comparison_expr->left()->type() == ExprType::SUB_QUERY) {
        LOG_WARN("Creating subquery physical operator for left child");
        auto sub_query_expr = static_cast<SubqueryExpr *>(comparison_expr->left().get());
        
        if (sub_query_expr->physical_operator() != nullptr) {
          LOG_WARN("[UNEXPECTED] subquery physical operator is not null!");
        }
        unique_ptr<PhysicalOperator> subquery_phy_oper = nullptr;
        RC rc = create(*sub_query_expr->logical_operator(), subquery_phy_oper,session);
        if (rc != RC::SUCCESS) {
          LOG_WARN("failed to create subquery physical operator. rc=%s", strrc(rc));
          return rc;
        }
        sub_query_expr->set_physical_operator(std::move(subquery_phy_oper));
      } 
      if (comparison_expr->right()->type() == ExprType::SUB_QUERY) {
        LOG_WARN("Creating subquery physical operator for right child");
        auto sub_query_expr = static_cast<SubqueryExpr *>(comparison_expr->right().get());
        if (sub_query_expr->physical_operator() != nullptr) {
          LOG_WARN("[UNEXPECTED] subquery physical operator is not null!");
        }
        unique_ptr<PhysicalOperator> subquery_phy_oper = nullptr;
        RC rc = create(*sub_query_expr->logical_operator(), subquery_phy_oper,session);
        if (rc != RC::SUCCESS) {
          LOG_WARN("failed to create subquery physical operator. rc=%s", strrc(rc));
          return rc;
        }
        sub_query_expr->set_physical_operator(std::move(subquery_phy_oper));
      }
      if (comparison_expr->comp() == NOT_EQUAL) {
        has_not_equal = true;
        break;  // 如果有NOT_EQUAL，不使用索引扫描
      }
      // 简单处理，就找等值查询
      if (comparison_expr->comp() != EQUAL_TO) {
        continue;
      }

      unique_ptr<Expression> &left_expr  = comparison_expr->left();
      unique_ptr<Expression> &right_expr = comparison_expr->right();
      // 左右比较的一边最少是一个值
      if (left_expr->type() != ExprType::VALUE && right_expr->type() != ExprType::VALUE) {
        continue;
      }

      FieldExpr *field_expr = nullptr;
      if (left_expr->type() == ExprType::FIELD) {
        ASSERT(right_expr->type() == ExprType::VALUE, "right expr should be a value expr while left is field expr");
        field_expr = static_cast<FieldExpr *>(left_expr.get());
        value_expr = static_cast<ValueExpr *>(right_expr.get());
      } else if (right_expr->type() == ExprType::FIELD) {
        ASSERT(left_expr->type() == ExprType::VALUE, "left expr should be a value expr while right is a field expr");
        field_expr = static_cast<FieldExpr *>(right_expr.get());
        value_expr = static_cast<ValueExpr *>(left_expr.get());
      }

      if (field_expr == nullptr) {
        continue;
      }

      const Field &field = field_expr->field();
      index              = table->find_index_by_field(field.field_name());
      if (nullptr != index) {
        break;
      }
    }
  }

  if (index != nullptr) {
    if (has_not_equal) {
      // 对于NOT_EQUAL查询，使用索引进行全表扫描，然后在过滤阶段应用条件
      IndexScanPhysicalOperator *index_scan_oper = new IndexScanPhysicalOperator(table,
          index,
          table_get_oper.read_write_mode(),
          nullptr,  // left_value = nullptr 表示全表扫描
          true,     // left_inclusive
          nullptr,  // right_value = nullptr 表示全表扫描
          true);    // right_inclusive

      index_scan_oper->set_predicates(std::move(predicates));
      oper = unique_ptr<PhysicalOperator>(index_scan_oper);
      LOG_TRACE("use index scan for NOT_EQUAL query (full table scan)");
    } else {
      // 对于等值查询，使用索引范围扫描
      ASSERT(value_expr != nullptr, "got an index but value expr is null ?");
      const Value               &value           = value_expr->get_value();
      IndexScanPhysicalOperator *index_scan_oper = new IndexScanPhysicalOperator(table,
          index,
          table_get_oper.read_write_mode(),
          &value,
          true /*left_inclusive*/,
          &value,
          true /*right_inclusive*/);

      index_scan_oper->set_predicates(std::move(predicates));
      oper = unique_ptr<PhysicalOperator>(index_scan_oper);
      LOG_TRACE("use index scan for EQUAL query");
    }
  } else {
    auto table_scan_oper = new TableScanPhysicalOperator(table, table_get_oper.read_write_mode());
    table_scan_oper->set_predicates(std::move(predicates));
    oper = unique_ptr<PhysicalOperator>(table_scan_oper);
    LOG_TRACE("use table scan");
  }

  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_plan(PredicateLogicalOperator &pred_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &children_opers = pred_oper.children();
  ASSERT(children_opers.size() == 1, "predicate logical operator's sub oper number should be 1");

  LogicalOperator &child_oper = *children_opers.front();

  unique_ptr<PhysicalOperator> child_phy_oper;
  RC                           rc = create(child_oper, child_phy_oper, session);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create child operator of predicate operator. rc=%s", strrc(rc));
    return rc;
  }

  vector<unique_ptr<Expression>> &expressions = pred_oper.expressions();
  ASSERT(expressions.size() == 1, "predicate logical operator's children should be 1");

  unique_ptr<Expression> &expression = expressions.front(); // Use reference instead of move
  
  // 取出子查询的逻辑算子，创建物理算子
  std::vector<ComparisonExpr *> comparison_exprs;
  if (expression->type() == ExprType::CONJUNCTION) {
    auto conjunction_expr = static_cast<ConjunctionExpr *>(expression.get());
    vector<unique_ptr<Expression>> &children = conjunction_expr->children();
    for (auto &child_expr : children) {
      if (child_expr->type() == ExprType::COMPARISON) {
        comparison_exprs.push_back(static_cast<ComparisonExpr *>(child_expr.get()));
      }
    }
  } else if (expression->type() == ExprType::COMPARISON) {
    comparison_exprs.push_back(static_cast<ComparisonExpr *>(expression.get()));
  }

  for (auto &comparison_expr : comparison_exprs) {
    if (comparison_expr->left()->type() == ExprType::SUB_QUERY) {
      auto sub_query_expr = static_cast<SubqueryExpr *>(comparison_expr->left().get());
      // 为子查询表达式设置事务上下文，保证后续 open/scan 的上下文有效
      if (session != nullptr) {
        sub_query_expr->set_trx(session->current_trx());
      }
      if (sub_query_expr->physical_operator() != nullptr) {
        LOG_WARN("[UNEXPECTED] subquery physical operator is not null!");
      }
      unique_ptr<PhysicalOperator> subquery_phy_oper = nullptr;
      RC rc = create(*sub_query_expr->logical_operator(), subquery_phy_oper, session);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to create subquery physical operator. rc=%s", strrc(rc));
        return rc;
      }
      sub_query_expr->set_physical_operator(std::move(subquery_phy_oper));
    }
    if (comparison_expr->right()->type() == ExprType::SUB_QUERY) {
      auto sub_query_expr = static_cast<SubqueryExpr *>(comparison_expr->right().get());
      // 为子查询表达式设置事务上下文，保证后续 open/scan 的上下文有效
      if (session != nullptr) {
        sub_query_expr->set_trx(session->current_trx());
      }
      if (sub_query_expr->physical_operator() != nullptr) {
        LOG_WARN("[UNEXPECTED] subquery physical operator is not null!");
      }
      unique_ptr<PhysicalOperator> subquery_phy_oper = nullptr;
      RC rc = create(*sub_query_expr->logical_operator(), subquery_phy_oper, session);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to create subquery physical operator. rc=%s", strrc(rc));
        return rc;
      }
      sub_query_expr->set_physical_operator(std::move(subquery_phy_oper));
    }
  }
  
  // Move the original expression into the predicate operator so that any
  // SubqueryExpr retains its already-built physical operators.
  unique_ptr<Expression> moved_expr = std::move(expression);
  oper = unique_ptr<PhysicalOperator>(new PredicatePhysicalOperator(std::move(moved_expr)));
  oper->add_child(std::move(child_phy_oper));
  return rc;
}

RC PhysicalPlanGenerator::create_plan(ProjectLogicalOperator &project_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = project_oper.children();

  unique_ptr<PhysicalOperator> child_phy_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();

    rc = create(*child_oper, child_phy_oper, session);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create project logical operator's child physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  auto project_operator = make_unique<ProjectPhysicalOperator>(std::move(project_oper.expressions()));
  if (child_phy_oper) {
    project_operator->add_child(std::move(child_phy_oper));
  }

  oper = std::move(project_operator);

  LOG_TRACE("create a project physical operator");
  return rc;
}

RC PhysicalPlanGenerator::create_plan(InsertLogicalOperator &insert_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  Table                  *table           = insert_oper.table();
  vector<Value>          &values          = insert_oper.values();
  InsertPhysicalOperator *insert_phy_oper = new InsertPhysicalOperator(table, std::move(values));
  oper.reset(insert_phy_oper);
  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_plan(DeleteLogicalOperator &delete_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = delete_oper.children();

  unique_ptr<PhysicalOperator> child_physical_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();

    rc = create(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  oper = unique_ptr<PhysicalOperator>(new DeletePhysicalOperator(delete_oper.table()));

  if (child_physical_oper) {
    oper->add_child(std::move(child_physical_oper));
  }
  return rc;
}

RC PhysicalPlanGenerator::create_plan(UpdateLogicalOperator &update_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = update_oper.children();

  unique_ptr<PhysicalOperator> child_physical_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();

    rc = create(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  oper = unique_ptr<PhysicalOperator>(new UpdatePhysicalOperator(update_oper.table(), update_oper.attribute_name(), update_oper.value()));

  if (child_physical_oper) {
    oper->add_child(std::move(child_physical_oper));
  }
  return rc;
}

RC PhysicalPlanGenerator::create_plan(ExplainLogicalOperator &explain_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = explain_oper.children();

  RC rc = RC::SUCCESS;

  unique_ptr<PhysicalOperator> explain_physical_oper(new ExplainPhysicalOperator);
  for (unique_ptr<LogicalOperator> &child_oper : child_opers) {
    unique_ptr<PhysicalOperator> child_physical_oper;
    rc = create(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create child physical operator. rc=%s", strrc(rc));
      return rc;
    }

    explain_physical_oper->add_child(std::move(child_physical_oper));
  }

  oper = std::move(explain_physical_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_plan(JoinLogicalOperator &join_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  vector<unique_ptr<LogicalOperator>> &child_opers = join_oper.children();
  if (child_opers.size() != 2) {
    LOG_WARN("join operator should have 2 children, but have %d", child_opers.size());
    return RC::INTERNAL;
  }
  //LOG_INFO("Session hash_join_on: %s", session->hash_join_on() ? "true" : "false");
  if (session->hash_join_on() && can_use_hash_join(join_oper)) {
    //LOG_INFO("Using Hash Join for this query");
    // 创建哈希连接操作符
    unique_ptr<HashJoinPhysicalOperator> hash_join_oper(new HashJoinPhysicalOperator());
    
    // 智能选择 JOIN 字段 - 找到与当前 JOIN 相关的等值条件
    const auto &join_predicates = join_oper.get_join_predicates();
    
    for (const auto &predicate : join_predicates) {
      auto *comp_expr = dynamic_cast<ComparisonExpr*>(predicate.get());
      if (comp_expr != nullptr && comp_expr->comp() == CompOp::EQUAL_TO) {
        auto *left_field = dynamic_cast<FieldExpr*>(comp_expr->left().get());
        auto *right_field = dynamic_cast<FieldExpr*>(comp_expr->right().get());
        if (left_field != nullptr && right_field != nullptr) {
         // LOG_WARN("HashJoin: Found join condition: %s.%s = %s.%s", 
                   left_field->field().table_name(), left_field->field().field_name(),
                   right_field->field().table_name(), right_field->field().field_name();
          // 检查字段是否与当前 JOIN 相关
          const char *left_table = left_field->field().table_name();
          const char *right_table = right_field->field().table_name();
          
          // 获取当前 JOIN 的子操作符
          const auto &children = join_oper.children();
          if (children.size() >= 2) {
            // 检查左字段是否来自左子树，右字段是否来自右子树
            bool left_from_left = false;
            bool right_from_right = false;
            
            // 检查左子树（可能是单个表或之前 JOIN 的结果）
            if (children[0]->type() == LogicalOperatorType::TABLE_GET) {
              auto *left_table_get = dynamic_cast<TableGetLogicalOperator*>(children[0].get());
              if (left_table_get != nullptr && strcmp(left_table_get->table()->name(), left_table) == 0) {
                left_from_left = true;
              }
            } else if (children[0]->type() == LogicalOperatorType::JOIN) {
              // 左子树是 JOIN 结果，检查是否包含左字段的表
              // 简化检查：如果左字段的表名在左子树中，认为来自左子树
              left_from_left = true; // 暂时简化，假设左字段来自左子树
            }
            
            // 检查右子树（当前表）
            if (children[1]->type() == LogicalOperatorType::TABLE_GET) {
              auto *right_table_get = dynamic_cast<TableGetLogicalOperator*>(children[1].get());
              if (right_table_get != nullptr && strcmp(right_table_get->table()->name(), right_table) == 0) {
                right_from_right = true;
              }
            }
            
            // 如果左字段来自左子树，右字段来自右子树，则使用这个条件
            if (left_from_left && right_from_right) {
              hash_join_oper->set_join_fields(left_field, right_field);
              break;
            }
          }
        }
      }
    }
    
    // 设置过滤条件 - 从 JoinLogicalOperator 的 predicate_op_ 中获取
    auto *predicate_op = join_oper.get_predicate_op();
    if (predicate_op != nullptr) {
      auto *predicate_oper = dynamic_cast<PredicateLogicalOperator*>(predicate_op);
      if (predicate_oper != nullptr) {
        hash_join_oper->set_filter_expressions(predicate_oper->expressions());
      }
    }
    
    for (auto &child_oper : child_opers) {
      unique_ptr<PhysicalOperator> child_physical_oper;
      rc = create(*child_oper, child_physical_oper, session);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to create physical child oper. rc=%s", strrc(rc));
        return rc;
      }
      hash_join_oper->add_child(std::move(child_physical_oper));
    }
    
    oper = std::move(hash_join_oper);
    //LOG_INFO("Created HashJoinPhysicalOperator");
  } else {
    LOG_INFO("Using Nested Loop Join for this query");
    unique_ptr<PhysicalOperator> join_physical_oper(new NestedLoopJoinPhysicalOperator());
    for (auto &child_oper : child_opers) {
      unique_ptr<PhysicalOperator> child_physical_oper;
      rc = create(*child_oper, child_physical_oper, session);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to create physical child oper. rc=%s", strrc(rc));
        return rc;
      }

      join_physical_oper->add_child(std::move(child_physical_oper));
    }

    oper = std::move(join_physical_oper);
  }
  return rc;
}

bool PhysicalPlanGenerator::can_use_hash_join(JoinLogicalOperator &join_oper)
{
  //LOG_INFO("Checking if can use hash join...");
  
  // 总是使用 Hash Join，因为它可以处理所有情况：
  // 1. 等值条件：使用正常的 Hash Join 算法
  // 2. 非等值条件：退化为 Nested Loop Join 行为
  // 3. 混合条件：Hash Join + 过滤
  return true;
}

RC PhysicalPlanGenerator::create_plan(CalcLogicalOperator &logical_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  CalcPhysicalOperator *calc_oper = new CalcPhysicalOperator(std::move(logical_oper.expressions()));
  oper.reset(calc_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_plan(GroupByLogicalOperator &logical_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  vector<unique_ptr<Expression>> &group_by_expressions = logical_oper.group_by_expressions();
  unique_ptr<GroupByPhysicalOperator> group_by_oper;
  if (group_by_expressions.empty()) {
    group_by_oper = make_unique<ScalarGroupByPhysicalOperator>(std::move(logical_oper.aggregate_expressions()));
  } else {
    group_by_oper = make_unique<HashGroupByPhysicalOperator>(std::move(logical_oper.group_by_expressions()),
        std::move(logical_oper.aggregate_expressions()));
  }

  ASSERT(logical_oper.children().size() == 1, "group by operator should have 1 child");

  LogicalOperator             &child_oper = *logical_oper.children().front();
  unique_ptr<PhysicalOperator> child_physical_oper;
  rc = create(child_oper, child_physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create child physical operator of group by operator. rc=%s", strrc(rc));
    return rc;
  }

  group_by_oper->add_child(std::move(child_physical_oper));

  oper = std::move(group_by_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_vec_plan(TableGetLogicalOperator &table_get_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<Expression>> &predicates = table_get_oper.predicates();
  Table *table = table_get_oper.table();
  TableScanVecPhysicalOperator *table_scan_oper = new TableScanVecPhysicalOperator(table, table_get_oper.read_write_mode());
  table_scan_oper->set_predicates(std::move(predicates));
  oper = unique_ptr<PhysicalOperator>(table_scan_oper);
  LOG_TRACE("use vectorized table scan");

  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_vec_plan(GroupByLogicalOperator &logical_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;
  unique_ptr<PhysicalOperator> physical_oper = nullptr;
  if (logical_oper.group_by_expressions().empty()) {
    physical_oper = make_unique<AggregateVecPhysicalOperator>(std::move(logical_oper.aggregate_expressions()));
  } else {
    physical_oper = make_unique<GroupByVecPhysicalOperator>(
      std::move(logical_oper.group_by_expressions()), std::move(logical_oper.aggregate_expressions()));

  }

  ASSERT(logical_oper.children().size() == 1, "group by operator should have 1 child");

  LogicalOperator             &child_oper = *logical_oper.children().front();
  unique_ptr<PhysicalOperator> child_physical_oper;
  rc = create_vec(child_oper, child_physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create child physical operator of group by(vec) operator. rc=%s", strrc(rc));
    return rc;
  }

  physical_oper->add_child(std::move(child_physical_oper));

  oper = std::move(physical_oper);
  return rc;

  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_vec_plan(ProjectLogicalOperator &project_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = project_oper.children();

  unique_ptr<PhysicalOperator> child_phy_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();
    rc                          = create_vec(*child_oper, child_phy_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create project logical operator's child physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  auto project_operator = make_unique<ProjectVecPhysicalOperator>(std::move(project_oper.expressions()));

  if (child_phy_oper != nullptr) {
    vector<Expression *> expressions;
    for (auto &expr : project_operator->expressions()) {
      expressions.push_back(expr.get());
    }
    auto expr_operator = make_unique<ExprVecPhysicalOperator>(std::move(expressions));
    expr_operator->add_child(std::move(child_phy_oper));
    project_operator->add_child(std::move(expr_operator));
  }

  oper = std::move(project_operator);

  LOG_TRACE("create a project physical operator");
  return rc;
}


RC PhysicalPlanGenerator::create_vec_plan(ExplainLogicalOperator &explain_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = explain_oper.children();

  RC rc = RC::SUCCESS;
  // reuse `ExplainPhysicalOperator` in explain vectorized physical plan
  unique_ptr<PhysicalOperator> explain_physical_oper(new ExplainPhysicalOperator);
  for (unique_ptr<LogicalOperator> &child_oper : child_opers) {
    unique_ptr<PhysicalOperator> child_physical_oper;
    rc = create_vec(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create child physical operator. rc=%s", strrc(rc));
      return rc;
    }

    explain_physical_oper->add_child(std::move(child_physical_oper));
  }

  oper = std::move(explain_physical_oper);
  return rc;
}

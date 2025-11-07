
%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log/log.h"
#include "common/lang/string.h"
#include "sql/parser/parse_defs.h"
#include "sql/parser/yacc_sql.hpp"
#include "sql/parser/lex_sql.h"
#include "sql/expr/expression.h"
#include "sql/expr/subquery_expr.h"

using namespace std;

// 全局变量用于存储 JOIN 条件
static vector<JoinConditionSqlNode> g_join_conditions;
static vector<TableReferenceSqlNode> g_table_references;

string token_name(const char *sql_string, YYLTYPE *llocp)
{
  return string(sql_string + llocp->first_column, llocp->last_column - llocp->first_column + 1);
}

int yyerror(YYLTYPE *llocp, const char *sql_string, ParsedSqlResult *sql_result, yyscan_t scanner, const char *msg)
{
  unique_ptr<ParsedSqlNode> error_sql_node = make_unique<ParsedSqlNode>(SCF_ERROR);
  error_sql_node->error.error_msg = msg;
  error_sql_node->error.line = llocp->first_line;
  error_sql_node->error.column = llocp->first_column;
  sql_result->add_sql_node(std::move(error_sql_node));
  return 0;
}

ArithmeticExpr *create_arithmetic_expression(ArithmeticExpr::Type type,
                                             Expression *left,
                                             Expression *right,
                                             const char *sql_string,
                                             YYLTYPE *llocp)
{
  ArithmeticExpr *expr = new ArithmeticExpr(type, left, right);
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

UnboundAggregateExpr *create_aggregate_expression(const char *aggregate_name,
                                           Expression *child,
                                           const char *sql_string,
                                           YYLTYPE *llocp)
{
  UnboundAggregateExpr *expr = new UnboundAggregateExpr(aggregate_name, child);
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

%}

%define api.pure full
%define parse.error verbose
/** 启用位置标识 **/
%locations
%lex-param { yyscan_t scanner }
/** 这些定义了在yyparse函数中的参数 **/
%parse-param { const char * sql_string }
%parse-param { ParsedSqlResult * sql_result }
%parse-param { void * scanner }

//标识tokens
%token  SEMICOLON
        BY
        CREATE
        DROP
        GROUP
        TABLE
        TABLES
        INDEX
        CALC
        SELECT
        ORDER  
        ASC   
        DESC
        SHOW
        SYNC
        INSERT
        DELETE
        UPDATE
        LBRACE
        RBRACE
        COMMA
        TRX_BEGIN
        TRX_COMMIT
        TRX_ROLLBACK
        INT_T
        STRING_T
        FLOAT_T
        DATE_T
        VECTOR_T
        HELP
        EXIT
        DOT //QUOTE
        INTO
        VALUES
        FROM
        WHERE
        AND
        SET
        ON
        LOAD
        DATA
        INFILE
        EXPLAIN
        STORAGE
        FORMAT
        AS
        INNER
        JOIN
        PRIMARY
        KEY
        ANALYZE
        FIELDS
        TERMINATED
        ENCLOSED
        EQ
        LT
        GT
        LE
        GE
        NE
        IN
        NULL_T
        NOT
        IS

/** union 中定义各种数据类型，真实生成的代码也是union类型，所以不能有非POD类型的数据 **/
%union {
  ParsedSqlNode *                            sql_node;
  ConditionSqlNode *                         condition;
  Value *                                    value;
  enum CompOp                                comp;
  RelAttrSqlNode *                           rel_attr;
  RelationSqlNode *                          relation_sql_node;
  vector<AttrInfoSqlNode> *                  attr_infos;
  AttrInfoSqlNode *                          attr_info;
  Expression *                               expression;
  vector<unique_ptr<Expression>> *           expression_list;
  vector<unique_ptr<OrderedUnboundFieldExpr>> *           order_expression_list;
  vector<Value> *                            value_list;
  vector<ConditionSqlNode> *                 condition_list;
  vector<RelAttrSqlNode> *                   rel_attr_list;
  vector<string> *                           relation_list;
  vector<string> *                           key_list;
  vector<JoinConditionSqlNode> *             join_condition_list;
  char *                                     cstring;
  int                                        number;  // 进一步用于null的表示
  float                                      floats;
}

%destructor { delete $$; } <condition>
%destructor { delete $$; } <value>
%destructor { delete $$; } <rel_attr>
%destructor { delete $$; } <attr_infos>
%destructor { delete $$; } <expression>
%destructor { delete $$; } <expression_list>
%destructor { delete $$; } <value_list>
%destructor { delete $$; } <condition_list>
// %destructor { delete $$; } <rel_attr_list>
%destructor { delete $$; } <relation_list>
%destructor { delete $$; } <key_list>

%token <number> NUMBER
%token <floats> FLOAT
%token <cstring> ID
%token <cstring> SSS
//非终结符

/** type 定义了各种解析后的结果输出的是什么类型。类型对应了 union 中的定义的成员变量名称 **/
%type <number>              type
%type <condition>           condition
%type <value>               value
%type <number>              number
%type <number>              null_spec
%type <comp>                comp_op
%type <rel_attr>            rel_attr
%type <attr_infos>          attr_def_list
%type <attr_info>           attr_def
%type <value_list>          value_list
%type <condition_list>      where
%type <condition_list>      condition_list
%type <cstring>             storage_format
%type <key_list>            primary_key
%type <key_list>            attr_list
%type <relation_list>       rel_list
%type <relation_sql_node>   relation
%type <expression>          expression
%type <expression>          aggregate_expression
%type <expression_list>     expression_list
%type <expression_list>     group_by
%type <order_expression_list>     order_by        // 整个order by子句
%type <order_expression_list>     order_by_list   // 多个排序项
%type <cstring>             fields_terminated_by
%type <cstring>             enclosed_by
%type <sql_node>            calc_stmt
%type <sql_node>            select_stmt
%type <sql_node>            insert_stmt
%type <sql_node>            update_stmt
%type <sql_node>            delete_stmt
%type <sql_node>            create_table_stmt
%type <sql_node>            drop_table_stmt
%type <sql_node>            analyze_table_stmt
%type <sql_node>            show_tables_stmt
%type <sql_node>            desc_table_stmt
%type <sql_node>            create_index_stmt
%type <sql_node>            drop_index_stmt
%type <sql_node>            sync_stmt
%type <sql_node>            begin_stmt
%type <sql_node>            commit_stmt
%type <sql_node>            rollback_stmt
%type <sql_node>            load_data_stmt
%type <sql_node>            explain_stmt
%type <sql_node>            set_variable_stmt
%type <sql_node>            help_stmt
%type <sql_node>            exit_stmt
%type <sql_node>            command_wrapper
// commands should be a list but I use a single command instead
%type <sql_node>            commands

// JOIN 相关类型
%type <cstring>             table_name
%type <condition>            join_condition
%type <join_condition_list>  join_condition_list
%type <sql_node>             subquery_stmt

%left '+' '-'
%left '*' '/'
%right UMINUS
%%

commands: command_wrapper opt_semicolon  //commands or sqls. parser starts here.
  {
    unique_ptr<ParsedSqlNode> sql_node = unique_ptr<ParsedSqlNode>($1);
    sql_result->add_sql_node(std::move(sql_node));
    
    // 清理全局变量
    g_join_conditions.clear();
    g_table_references.clear();
  }
  ;

command_wrapper:
    calc_stmt
  | select_stmt
  | insert_stmt
  | update_stmt
  | delete_stmt
  | create_table_stmt
  | drop_table_stmt
  | analyze_table_stmt
  | show_tables_stmt
  | desc_table_stmt
  | create_index_stmt
  | drop_index_stmt
  | sync_stmt
  | begin_stmt
  | commit_stmt
  | rollback_stmt
  | load_data_stmt
  | explain_stmt
  | set_variable_stmt
  | help_stmt
  | exit_stmt
    ;

exit_stmt:      
    EXIT {
      (void)yynerrs;  // 这么写为了消除yynerrs未使用的告警。如果你有更好的方法欢迎提PR
      $$ = new ParsedSqlNode(SCF_EXIT);
    };

help_stmt:
    HELP {
      $$ = new ParsedSqlNode(SCF_HELP);
    };

sync_stmt:
    SYNC {
      $$ = new ParsedSqlNode(SCF_SYNC);
    }
    ;

begin_stmt:
    TRX_BEGIN  {
      $$ = new ParsedSqlNode(SCF_BEGIN);
    }
    ;

commit_stmt:
    TRX_COMMIT {
      $$ = new ParsedSqlNode(SCF_COMMIT);
    }
    ;

rollback_stmt:
    TRX_ROLLBACK  {
      $$ = new ParsedSqlNode(SCF_ROLLBACK);
    }
    ;

drop_table_stmt:    /*drop table 语句的语法解析树*/
    DROP TABLE ID {
      $$ = new ParsedSqlNode(SCF_DROP_TABLE);
      $$->drop_table.relation_name = $3;
    };

analyze_table_stmt:  /* analyze table 语法的语法解析树*/
    ANALYZE TABLE ID {
      $$ = new ParsedSqlNode(SCF_ANALYZE_TABLE);
      $$->analyze_table.relation_name = $3;
    }
    ;

show_tables_stmt:
    SHOW TABLES {
      $$ = new ParsedSqlNode(SCF_SHOW_TABLES);
    }
    ;

desc_table_stmt:
    DESC ID  {
      $$ = new ParsedSqlNode(SCF_DESC_TABLE);
      $$->desc_table.relation_name = $2;
    }
    ;

create_index_stmt:    /*create index 语句的语法解析树*/
    CREATE INDEX ID ON ID LBRACE ID RBRACE
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $3;
      create_index.relation_name = $5;
      create_index.attribute_name = $7;
    }
    ;

drop_index_stmt:      /*drop index 语句的语法解析树*/
    DROP INDEX ID ON ID
    {
      $$ = new ParsedSqlNode(SCF_DROP_INDEX);
      $$->drop_index.index_name = $3;
      $$->drop_index.relation_name = $5;
    }
    ;
create_table_stmt:    /*create table 语句的语法解析树*/
    CREATE TABLE ID LBRACE attr_def_list primary_key RBRACE storage_format
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      //free($3);

      create_table.attr_infos.swap(*$5);
      delete $5;

      if ($6 != nullptr) {
        create_table.primary_keys.swap(*$6);
        delete $6;
      }
      if ($8 != nullptr) {
        create_table.storage_format = $8;
      }
    }
    ;
    
attr_def_list:
    attr_def
    {
      $$ = new vector<AttrInfoSqlNode>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | attr_def_list COMMA attr_def
    {
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;

// 表字段的定义
attr_def:
    ID type LBRACE number RBRACE null_spec
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = $4;
      $$->nullable = ($6 == 1);
    }
    | ID type null_spec
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = 4;
      $$->nullable = ($3 == 1);
    }
    ;

number:
    NUMBER {$$ = $1;}
    ;
type:
    INT_T      { $$ = static_cast<int>(AttrType::INTS); }
    | STRING_T { $$ = static_cast<int>(AttrType::CHARS); }
    | FLOAT_T  { $$ = static_cast<int>(AttrType::FLOATS); }
    | DATE_T   { $$ = static_cast<int>(AttrType::DATES); } 
    | VECTOR_T { $$ = static_cast<int>(AttrType::VECTORS); }
    ;
primary_key:
    /* empty */
    {
      $$ = nullptr;
    }
    | COMMA PRIMARY KEY LBRACE attr_list RBRACE
    {
      $$ = $5;
    }
    ;

attr_list:
    ID {
      $$ = new vector<string>();
      $$->push_back($1);
    }
    | ID COMMA attr_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<string>;
      }

      $$->insert($$->begin(), $1);
    }
    ;

null_spec:
    /* empty */ 
    {
      /* 省略时默认允许 NULL */
      $$ = 1; /* 1 表示 nullable */
    }
    | NULL_T 
    {
      $$ = 1; /* 明确写 NULL */
    }
    | NOT NULL_T
    {
      $$ = 0; /* NOT NULL => 不可空 */
    }
    ;


insert_stmt:        /* insert 语句的语法解析树 --> 暂时只支持一次性插入一个元组 --> 一个Value代表一个字段的值 */
    INSERT INTO ID VALUES LBRACE value_list RBRACE 
    {
      $$ = new ParsedSqlNode(SCF_INSERT);
      $$->insertion.relation_name = $3;
      $$->insertion.values.swap(*$6);
      delete $6;
    }
    ;

value_list:
    value
    {
      $$ = new vector<Value>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | value_list COMMA value { 
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;

// 表示插入的元组
value:
    NUMBER {
      $$ = new Value((int)$1);
      @$ = @1;
    }
    | FLOAT {
      $$ = new Value((float)$1);
      @$ = @1;
    }
    | SSS {
      char *tmp = common::substr($1,1,strlen($1)-2);
      $$ = new Value(tmp);
      free(tmp);
    }
    | NULL_T {
      $$ = new Value(); 
      /* 其他属性为空 & null = true --> 表示 null  */
      $$->set_null(true);
      @$ = @1;
    }
    ;


storage_format:
    /* empty */
    {
      $$ = nullptr;
    }
    | STORAGE FORMAT EQ ID
    {
      $$ = $4;
    }
    ;
    
delete_stmt:    /*  delete 语句的语法解析树*/
    DELETE FROM ID where 
    {
      $$ = new ParsedSqlNode(SCF_DELETE);
      $$->deletion.relation_name = $3;
      if ($4 != nullptr) {
        $$->deletion.conditions.swap(*$4);
        delete $4;
      }
    }
    ;
update_stmt:      /*  update 语句的语法解析树*/
    UPDATE ID SET ID EQ value where 
    {
      $$ = new ParsedSqlNode(SCF_UPDATE);
      $$->update.relation_name = $2;
      $$->update.attribute_name = $4;
      $$->update.value = *$6;
      if ($7 != nullptr) {
        $$->update.conditions.swap(*$7);
        delete $7;
      }
    }
    ;
subquery_stmt: 
    SELECT expression_list FROM rel_list where group_by
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }

      if ($4 != nullptr) {
        $$->selection.relations.swap(*$4);
        delete $4;
      }

      if ($5 != nullptr) {
        $$->selection.conditions.swap(*$5);
        delete $5;
      }

      if ($6 != nullptr) {
        $$->selection.group_by.swap(*$6);
        delete $6;
      }
      
      // 只使用子查询自己的表引用（从 rel_list 解析时添加到 g_table_references 的表）
      // 计算子查询自己的表引用数量（基于 relations 的数量）
      size_t subquery_table_count = $$->selection.relations.size();
      
      // 从 g_table_references 的末尾提取子查询自己的表引用
      if (subquery_table_count > 0 && g_table_references.size() >= subquery_table_count) {
        // 提取最后 subquery_table_count 个表引用（这些是子查询自己的表）
        vector<TableReferenceSqlNode> subquery_table_refs;
        subquery_table_refs.insert(
          subquery_table_refs.end(),
          g_table_references.end() - subquery_table_count,
          g_table_references.end()
        );
        $$->selection.table_references = subquery_table_refs;
        
        // 同时填充 ALIASES 字段
        for (const auto &ref : subquery_table_refs) {
          RelationSqlNode alias_node;
          alias_node.name = ref.table_name;
          alias_node.alias = ref.alias;
          $$->selection.ALIASES.push_back(alias_node);
        }
        
        // 从 g_table_references 中移除子查询的表引用，恢复外层查询的状态
        g_table_references.erase(
          g_table_references.end() - subquery_table_count,
          g_table_references.end()
        );
      }
      
      // 清空 JOIN 条件相关的全局变量（只清空 join_conditions，table_references 已恢复）
      g_join_conditions.clear();
    }
    ;

select_stmt:        /*  select 语句的语法解析树*/
    SELECT expression_list FROM rel_list where group_by order_by
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }

      if ($4 != nullptr) {
        $$->selection.relations.swap(*$4);
        delete $4;
      }

      if ($5 != nullptr) {
        $$->selection.conditions.swap(*$5);
        delete $5;
      }

      if ($6 != nullptr) {
        $$->selection.group_by.swap(*$6);
        delete $6;
      }

      // 把解析得到的列表放入SQL Node的order_by属性中存起来
      if ($7 != nullptr) {
        $$->selection.order_by.swap(*$7);
        delete $7;
      }
      
      // 注意：由于 yacc 是递归下降解析，当执行到这里时：
      // 1. rel_list 已经解析完成，把当前查询的表添加到 g_table_references
      // 2. where 子句也已经解析完成，如果包含子查询，子查询的表引用已经被添加到 g_table_references 然后又被移除
      // 所以，g_table_references 现在应该只包含当前查询的表引用
      // 但是，为了安全起见，我们只使用 relations 中的表来构建 table_references
      // 因为 relations 是直接从 FROM 子句解析而来的，不会被子查询污染
      
      // 基于 relations 构建 table_references 和 ALIASES
      // 这样可以避免 g_table_references 被子查询污染的问题
      if (!$$->selection.relations.empty()) {
        // 从 g_table_references 中查找与 relations 匹配的表引用
        // 注意：我们需要从 g_table_references 的末尾向前查找，因为当前查询的表应该在末尾
        size_t current_query_table_count = $$->selection.relations.size();
        size_t start_pos = (g_table_references.size() >= current_query_table_count) 
                          ? (g_table_references.size() - current_query_table_count) 
                          : 0;
        
        // 构建一个 relations 的集合，用于快速查找
        unordered_set<string> relations_set($$->selection.relations.begin(), $$->selection.relations.end());
        
        // 从 g_table_references 的末尾向前查找匹配的表引用
        vector<TableReferenceSqlNode> current_query_table_refs;
        for (size_t i = g_table_references.size(); i > start_pos && current_query_table_refs.size() < current_query_table_count; ) {
          --i;
          const auto &ref = g_table_references[i];
          if (relations_set.count(ref.table_name)) {
            current_query_table_refs.insert(current_query_table_refs.begin(), ref);
          }
        }
        
        // 如果从 g_table_references 中找到的表引用数量不够，说明可能被污染了
        // 这种情况下，我们直接基于 relations 构建 table_references（不包含别名信息）
        if (current_query_table_refs.size() < current_query_table_count) {
          // 清空并重新构建
          current_query_table_refs.clear();
          for (const auto &rel_name : $$->selection.relations) {
            TableReferenceSqlNode ref;
            ref.table_name = rel_name;
            ref.alias = "";  // 别名信息可能丢失，但至少表名是正确的
            current_query_table_refs.push_back(ref);
          }
        }
        
        $$->selection.table_references = current_query_table_refs;
        
        // 同时填充 ALIASES 字段
        for (const auto& table_ref : current_query_table_refs) {
          RelationSqlNode alias_node;
          alias_node.name = table_ref.table_name;
          alias_node.alias = table_ref.alias;
          $$->selection.ALIASES.push_back(alias_node);
        }
        
        // 从 g_table_references 中移除当前查询的表引用，恢复外层查询的状态
        // 注意：我们需要移除与 relations 匹配的表引用，而不是简单地移除末尾的表引用
        // 因为 g_table_references 可能已经被子查询污染
        for (auto it = g_table_references.begin(); it != g_table_references.end(); ) {
          if (relations_set.count(it->table_name)) {
            it = g_table_references.erase(it);
          } else {
            ++it;
          }
        }
      }
      // 如果 relations 为空，说明可能是子查询或其他情况，不清空 g_table_references
    }
    ;
calc_stmt:
    CALC expression_list
    {
      $$ = new ParsedSqlNode(SCF_CALC);
      $$->calc.expressions.swap(*$2);
      delete $2;
    }
    ;

expression_list:
    expression
    {
      $$ = new std::vector<std::unique_ptr<Expression>>;
      $$->emplace_back($1);
    }
    | expression ID{
      if ($1 != nullptr && $2 != nullptr) {
        $$ = new std::vector<std::unique_ptr<Expression>>;
        $1->set_alias(std::string($2));
        $$->emplace_back($1);
      } else {
        $$ = nullptr;
      }
    }
    | expression AS ID{
      if ($1 != nullptr && $3 != nullptr) {
        $$ = new std::vector<std::unique_ptr<Expression>>;
        $1->set_alias(std::string($3));
        $$->emplace_back($1);
      } else {
        $$ = nullptr;
      }
    }
    | expression ID COMMA expression_list
    {
      if ($1 != nullptr && $2 != nullptr) {
        if ($4 != nullptr) {
          $$ = $4;
        } else {
          $$ = new std::vector<std::unique_ptr<Expression>>;
        }
        $1->set_alias(std::string($2));
        $$->emplace($$->begin(), $1);
      } else {
        $$ = $4;
      }
    }
    | expression AS ID COMMA expression_list
    {
      if ($1 != nullptr && $3 != nullptr) {
        if ($5 != nullptr) {
          $$ = $5;
        } else {
          $$ = new std::vector<std::unique_ptr<Expression>>;
        }
        $1->set_alias(std::string($3));
        $$->emplace($$->begin(), $1);
      } else {
        $$ = $5;
      }
    }
    | expression COMMA expression_list
    {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new std::vector<std::unique_ptr<Expression>>;
      }
      $$->emplace($$->begin(), $1);
    }
    ;
expression:
    expression '+' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::ADD, $1, $3, sql_string, &@$);
    }
    | expression '-' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::SUB, $1, $3, sql_string, &@$);
    }
    | expression '*' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::MUL, $1, $3, sql_string, &@$);
    }
    | expression '/' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::DIV, $1, $3, sql_string, &@$);
    }
    | LBRACE expression RBRACE {
      $$ = $2;
      $$->set_name(token_name(sql_string, &@$));
    }
    | '-' expression %prec UMINUS {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::NEGATIVE, $2, nullptr, sql_string, &@$);
    }
    | '*' {
      $$ = new StarExpr();
    }
    | value {
      $$ = new ValueExpr(*$1);
      $$->set_name(token_name(sql_string, &@$));
      delete $1;
    }
    | ID DOT '*' {
      // 支持表限定的星号：t2.*
      $$ = new StarExpr($1);
      $$->set_name(token_name(sql_string, &@$));
    }
    | rel_attr {
      RelAttrSqlNode *node = $1;
      $$ = new UnboundFieldExpr(node->relation_name, node->attribute_name);
      $$->set_name(token_name(sql_string, &@$));
      delete $1;
    }
    | aggregate_expression {
      $$ = $1;
    }
    | LBRACE select_stmt RBRACE {
      // 子查询表达式
      $$ = new SubqueryExpr($2);
      $$->set_name(token_name(sql_string, &@$));
    }
    ;

aggregate_expression:
    ID LBRACE expression RBRACE {
      $$ = create_aggregate_expression($1, $3, sql_string, &@$);
    }
    ;

rel_attr:
    ID {
      $$ = new RelAttrSqlNode;
      $$->attribute_name = $1;
    }
    | ID DOT ID {
      $$ = new RelAttrSqlNode;
      $$->relation_name  = $1;
      $$->attribute_name = $3;
    }
    ;

relation:
    ID {
      $$ = new RelationSqlNode;
      $$->name = $1;
    }
    | ID AS ID {
      $$ = new RelationSqlNode;
      $$->name = $1;
      $$->alias = $3;
    }
    | ID ID {
      $$ = new RelationSqlNode;
      $$->name = $1;
      $$->alias = $2;
    }
    ;
rel_list:
    relation INNER JOIN relation ON join_condition_list {
      $$ = new vector<string>();
      $$->push_back($1->name);
      $$->push_back($4->name);
      
      // 创建表引用并存储 JOIN 条件
      TableReferenceSqlNode left_table;
      left_table.table_name = $1->name;
      left_table.alias = $1->alias;
      left_table.is_join = false;
      
      TableReferenceSqlNode right_table;
      right_table.table_name = $4->name;
      right_table.alias = $4->alias;
      right_table.is_join = true;
      right_table.join_conditions = *$6;
      
      g_table_references.push_back(left_table);
      g_table_references.push_back(right_table);
      
      delete $1;
      delete $4;
      delete $6;
    }
    | relation {
      $$ = new vector<string>();
      $$->push_back($1->name);
      // 存储表引用信息到全局变量
      TableReferenceSqlNode table_ref;
      table_ref.table_name = $1->name;
      table_ref.alias = $1->alias;
      g_table_references.push_back(table_ref);
      delete $1;
    }
    | relation COMMA rel_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<string>;
      }
      $$->insert($$->begin(), $1->name);
      // 存储表引用信息到全局变量
      TableReferenceSqlNode table_ref;
      table_ref.table_name = $1->name;
      table_ref.alias = $1->alias;
      g_table_references.push_back(table_ref);
      delete $1;
    }
    | rel_list INNER JOIN relation ON join_condition_list {
      if ($1 != nullptr) {
        $$ = $1;
      } else {
        $$ = new vector<string>;
      }
      $$->push_back($4->name);
      
      // 创建表引用并存储 JOIN 条件
      TableReferenceSqlNode right_table;
      right_table.table_name = $4->name;
      right_table.alias = $4->alias;
      right_table.is_join = true;
      right_table.join_conditions = *$6;
      
      g_table_references.push_back(right_table);
      
      delete $4;
      delete $6;
    }
    | rel_list COMMA relation {
      if ($1 != nullptr) {
        $$ = $1;
      } else {
        $$ = new vector<string>;
      }
      $$->push_back($3->name);
      
      // 存储表引用信息到全局变量
      TableReferenceSqlNode table_ref;
      table_ref.table_name = $3->name;
      table_ref.alias = $3->alias;
      g_table_references.push_back(table_ref);
      delete $3;
    }
    ;

// 添加 JOIN 条件处理规则
join_condition:
    rel_attr comp_op rel_attr {
      $$ = new ConditionSqlNode();
      $$->left_is_attr = true;
      $$->left_attr = *$1;
      $$->right_is_attr = true;
      $$->right_attr = *$3;
      $$->comp = $2;
      delete $1;
      delete $3;
    }
    | rel_attr comp_op value {
      $$ = new ConditionSqlNode();
      $$->left_is_attr = true;
      $$->left_attr = *$1;
      $$->right_is_attr = false;
      $$->right_value = *$3;
      $$->comp = $2;
      delete $1;
      delete $3;
    }
    | value comp_op rel_attr {
      $$ = new ConditionSqlNode();
      $$->left_is_attr = false;
      $$->left_value = *$1;
      $$->right_is_attr = true;
      $$->right_attr = *$3;
      $$->comp = $2;
      delete $1;
      delete $3;
    }
    ;

// 添加 JOIN 条件列表处理
join_condition_list:
    join_condition {
      $$ = new vector<JoinConditionSqlNode>();
      JoinConditionSqlNode join_cond;
      join_cond.left_is_attr = $1->left_is_attr;
      join_cond.left_attr = $1->left_attr;
      join_cond.left_value = $1->left_value;
      join_cond.right_is_attr = $1->right_is_attr;
      join_cond.right_attr = $1->right_attr;
      join_cond.right_value = $1->right_value;
      join_cond.comp = $1->comp;
      $$->push_back(join_cond);
      delete $1;
    }
    | join_condition_list AND join_condition {
      if ($1 != nullptr) {
        $$ = $1;
      } else {
        $$ = new vector<JoinConditionSqlNode>();
      }
      JoinConditionSqlNode join_cond;
      join_cond.left_is_attr = $3->left_is_attr;
      join_cond.left_attr = $3->left_attr;
      join_cond.left_value = $3->left_value;
      join_cond.right_is_attr = $3->right_is_attr;
      join_cond.right_attr = $3->right_attr;
      join_cond.right_value = $3->right_value;
      join_cond.comp = $3->comp;
      $$->push_back(join_cond);
      delete $3;
    }
    ;

table_name:
    ID {
      $$ = $1;
    }
    ;

table_references:
    ID
    {

    }

where:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE condition_list {
      $$ = $2;  
    }
    ;

condition_list:
    /* empty */
    {
      $$ = nullptr;
    }
    | condition {
      $$ = new vector<ConditionSqlNode>;
      $$->push_back(std::move(*$1));
      delete $1;
    }
    | condition AND condition_list {
      $$ = $3;
      $$->push_back(std::move(*$1));
      delete $1;
    }
    ;

condition:
    // 按照比较的不同表达形式进行分类
    rel_attr comp_op value
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_value = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | value comp_op value 
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_value = *$1;
      $$->right_is_attr = 0;
      $$->right_value = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | rel_attr comp_op rel_attr
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 1;
      $$->right_attr = *$3;
      $$->comp = $2;

      delete $1;
      delete $3;
    }
    | expression comp_op expression
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_expr = $1;
      $$->right_is_attr = 0;
      $$->right_expr = $3;
      $$->comp = $2;
    }
    | rel_attr IN LBRACE subquery_stmt RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_expr =new SubqueryExpr($4);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = IN_OP;
      delete $1;
    }
    | rel_attr NOT IN LBRACE subquery_stmt RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_expr =new SubqueryExpr($5);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = NOT_IN_OP;
      delete $1;
    }
    | expression IN LBRACE subquery_stmt RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_expr = $1;
      $$->right_is_attr = 0;
      $$->right_expr =new SubqueryExpr($4);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = IN_OP;
    }
    | expression NOT IN LBRACE subquery_stmt RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_expr = $1;
      $$->right_is_attr = 0;
      $$->right_expr =new SubqueryExpr($5);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = NOT_IN_OP;
    }
    | rel_attr IN LBRACE value_list RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_expr = new ValueListExpr($4);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = IN_OP;
      delete $1;
    }
    | rel_attr NOT IN LBRACE value_list RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 1;
      $$->left_attr = *$1;
      $$->right_is_attr = 0;
      $$->right_expr = new ValueListExpr($5);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = NOT_IN_OP;
      delete $1;
    }
    | expression IN LBRACE value_list RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_expr = $1;
      $$->right_is_attr = 0;
      $$->right_expr = new ValueListExpr($4);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = IN_OP;
    }
    | expression NOT IN LBRACE value_list RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_is_attr = 0;
      $$->left_expr = $1;
      $$->right_is_attr = 0;
      $$->right_expr = new ValueListExpr($5);
      $$->right_expr->set_name(token_name(sql_string, &@$));
      $$->comp = NOT_IN_OP;
    }
    | rel_attr IS NULL_T
    {
        $$ = new ConditionSqlNode;
        $$->left_is_attr = 1;
        $$->left_attr = *$1;

        $$->right_is_attr = 0;
        $$->right_value = Value(); // 空值
        $$->right_value.set_null(true);
        
        // 需要在 CompOp 中定义
        $$->comp = IS_OP;   
        delete $1;
    }
    | rel_attr IS NOT NULL_T
    {
        $$ = new ConditionSqlNode;
        $$->left_is_attr = 1;
        $$->left_attr = *$1;
         
        $$->right_is_attr = 0;
        $$->right_value = Value();
        $$->right_value.set_null(true);

        // 需要在 CompOp 中定义
        $$->comp = IS_NOT_OP; 
        // 此时NULL_T为纯标识符，没有需要delete的对象
        delete $1;
    }
    // 支持直接使用const is null / const is not null的形式 --> 直接判断即可
    | value IS NULL_T
    {
        $$ = new ConditionSqlNode;
        $$->left_is_attr = 0;
        $$->left_value = *$1;
        
        $$->right_is_attr = 0;
        $$->right_value = Value();
        $$->right_value.set_null(true);

        $$->comp = IS_OP;   
        delete $1;
    }
    | value IS NOT NULL_T
    {
        $$ = new ConditionSqlNode;
        $$->left_is_attr = 0;
        $$->left_value = *$1;

        $$->right_is_attr = 0;
        $$->right_value = Value();
        $$->right_value.set_null(true);

        $$->comp = IS_NOT_OP;  
        delete $1;
    }
    ;

// 枚举运算符token（识别符号得到），is / is not不加入该体系，直接赋值
comp_op:
      EQ { $$ = EQUAL_TO; }
    | LT { $$ = LESS_THAN; }
    | GT { $$ = GREAT_THAN; }
    | LE { $$ = LESS_EQUAL; }
    | GE { $$ = GREAT_EQUAL; }
    | NE { $$ = NOT_EQUAL; }
    | IN { $$ = IN_OP; }
    | NOT IN { $$ = NOT_IN_OP; }
    ;

// your code here
group_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | GROUP BY expression_list
    {
      // group by 的表达式范围与select查询值的表达式范围是不同的，比如group by不支持 *
      // 但是这里没有处理。
      $$ = $3;
    }
    ;

order_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ORDER BY order_by_list
    {
      // 存在该子句，则返回的语义值直接取order_by_list
      $$ = $3;
    }
    ;

// 解析order_by_list的语义值 --> 使用Expression存储排序字段
order_by_list:
    // 只包含一个项 --> rel_attr指的是table.id ???
    rel_attr
    {
      $$ = new vector<unique_ptr<OrderedUnboundFieldExpr>>;  
      auto expr = make_unique<OrderedUnboundFieldExpr>($1->relation_name, $1->attribute_name);
      expr->set_name($1->attribute_name);
      expr->set_order(1); // 默认 ASC
      $$->emplace_back(std::move(expr));
      // 释放 rel_attr 在解析时分配的内存
      delete $1;
    }
    // 显式定义了ASC或者DESC
    | rel_attr ASC
    {
      $$ = new vector<unique_ptr<OrderedUnboundFieldExpr>>;
      auto expr = make_unique<OrderedUnboundFieldExpr>($1->relation_name, $1->attribute_name);
      expr->set_name($1->attribute_name);
      expr->set_order(1); // ASC = 1
      $$->emplace_back(std::move(expr));
      delete $1;
    }
    | rel_attr DESC
    {
      $$ = new vector<unique_ptr<OrderedUnboundFieldExpr>>;
      auto expr = make_unique<OrderedUnboundFieldExpr>($1->relation_name, $1->attribute_name);
      expr->set_name($1->attribute_name);
      expr->set_order(-1); // DESC = -1
      $$->emplace_back(std::move(expr));
      delete $1;
    }
    | rel_attr ASC COMMA order_by_list
    {
      $$ = $4;
      auto expr = make_unique<OrderedUnboundFieldExpr>($1->relation_name, $1->attribute_name);
      expr->set_name($1->attribute_name);
      expr->set_order(1);
      // 通过回溯，保证字段在vector中的顺序与排序用的顺序一致
      $$->insert($$->begin(), std::move(expr));
      delete $1;
    }
    | rel_attr DESC COMMA order_by_list
    {
      $$ = $4;
      auto expr = make_unique<OrderedUnboundFieldExpr>($1->relation_name, $1->attribute_name);
      expr->set_name($1->attribute_name);
      expr->set_order(-1); 
      $$->insert($$->begin(), std::move(expr));
      delete $1;
    }
    | rel_attr COMMA order_by_list
    {
      $$ = $3;
      auto expr = make_unique<OrderedUnboundFieldExpr>($1->relation_name, $1->attribute_name);
      expr->set_name($1->attribute_name);
      expr->set_order(1);
      $$->insert($$->begin(), std::move(expr));
      delete $1;
    }
    ;

load_data_stmt:
    LOAD DATA INFILE SSS INTO TABLE ID fields_terminated_by enclosed_by
    {
      char *tmp_file_name = common::substr($4, 1, strlen($4) - 2);
      
      $$ = new ParsedSqlNode(SCF_LOAD_DATA);
      $$->load_data.relation_name = $7;
      $$->load_data.file_name = tmp_file_name;
      if ($8 != nullptr) {
        char *tmp = common::substr($8,1,strlen($8)-2);
        $$->load_data.terminated = $8;
        free(tmp);
      }
      if ($9 != nullptr) {
        char *tmp = common::substr($9,1,strlen($9)-2);
        $$->load_data.enclosed = $9;
        free(tmp);
      }
      free(tmp_file_name);
    }
    ;

fields_terminated_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | FIELDS TERMINATED BY SSS
    {
      $$ = $4;
    };

enclosed_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ENCLOSED BY SSS
    {
      $$ = $3;
    };

explain_stmt:
    EXPLAIN command_wrapper
    {
      $$ = new ParsedSqlNode(SCF_EXPLAIN);
      $$->explain.sql_node = unique_ptr<ParsedSqlNode>($2);
    }
    ;

set_variable_stmt:
    SET ID EQ value
    {
      $$ = new ParsedSqlNode(SCF_SET_VARIABLE);
      $$->set_variable.name  = $2;
      $$->set_variable.value = *$4;
      delete $4;
    }
    ;

opt_semicolon: /*empty*/
    | SEMICOLON
    ;
%%
//_____________________________________________________________________
extern void scan_string(const char *str, yyscan_t scanner);

int sql_parse(const char *s, ParsedSqlResult *sql_result) {
  yyscan_t scanner;
  std::vector<char *> allocated_strings;
  yylex_init_extra(static_cast<void*>(&allocated_strings),&scanner);
  scan_string(s, scanner);
  int result = yyparse(s, sql_result, scanner);

  for (char *ptr : allocated_strings) {
    free(ptr);
  }
  allocated_strings.clear();

  yylex_destroy(scanner);
  return result;
}
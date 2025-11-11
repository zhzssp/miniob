#pragma once

#include "sql/stmt/stmt.h"
#include "storage/table/table.h"
#include "common/lang/string.h"
#include "sql/stmt/stmt.h"

struct DropIndexSqlNode;

/**
 * @brief 创建索引的语句
 * @ingroup Statement
 */
class DropIndexStmt : public Stmt
{
public:
  DropIndexStmt(Table *table, string table_name, string index_name): table_(table), table_name_(table_name), index_name_(index_name){}

  virtual ~DropIndexStmt() = default;

  StmtType type() const override { return StmtType::DROP_INDEX; }

  Table *table() { return table_; }
  const char *table_name() const { return table_name_.c_str(); } 
  const char *index_name() const { return index_name_.c_str(); }

public:
  static RC create(Db *db, const DropIndexSqlNode &drop_index, Stmt *&stmt);

private:
  Table *table_;
  string table_name_;
  string index_name_;
};

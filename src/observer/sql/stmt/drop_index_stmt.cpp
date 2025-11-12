#include "sql/stmt/drop_index_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

using namespace std;
using namespace common;

RC DropIndexStmt::create(Db *db, const DropIndexSqlNode &drop_index, Stmt *&stmt)
{
  stmt = nullptr;

  const char *table_name = drop_index.relation_name.c_str();
  const char *index_name = drop_index.index_name.c_str();
  if (is_blank(table_name) || is_blank(index_name)) {
    LOG_WARN("invalid argument. db=%p, table_name=%p, index name=%s",
        db, table_name, index_name);
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // check whether the index exists
  Index *index = table->find_index(index_name);
  if (nullptr == index) {
    LOG_WARN("index with name(%s) does not exist. table name=%s", index_name, table_name);
    return RC::INDEX_NOT_EXISTS;
  }

  stmt = new DropIndexStmt(table, string(table_name), string(index_name));
  return RC::SUCCESS;
}
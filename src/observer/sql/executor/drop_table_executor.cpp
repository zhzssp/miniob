#include "sql/executor/create_table_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/drop_table_stmt.h"
#include "sql/executor/drop_table_executor.h"
#include "storage/db/db.h"

RC DropTableExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();
  ASSERT(stmt->type() == StmtType::DROP_TABLE,            // to define --> already defined
      "drop table executor can not run this command: %d",
      static_cast<int>(stmt->type()));
  
  // DropTableStmt只含有table_name属性
  DropTableStmt *drop_table_stmt = static_cast<DropTableStmt *>(stmt);  

  const char *table_name = drop_table_stmt->table_name().c_str();
  // 获取操作状态码
  LOG_INFO("Begin to execute drop table command !");
  RC rc = session->get_current_db()->drop_table(table_name);  // drop_table()待定义

  return rc;
}
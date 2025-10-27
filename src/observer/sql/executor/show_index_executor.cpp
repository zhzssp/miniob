#include "sql/executor/show_index_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/operator/string_list_physical_operator.h"
#include "sql/stmt/show_index_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

using namespace std;

RC ShowIndexExecutor::execute(SQLStageEvent *sql_event)
{
  RC            rc            = RC::SUCCESS;
  Stmt         *stmt          = sql_event->stmt();
  SessionEvent *session_event = sql_event->session_event();
  Session      *session       = session_event->session();
  ASSERT(stmt->type() == StmtType::SHOW_INDEX,
      "show index executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  ShowIndexStmt *show_index_stmt = static_cast<ShowIndexStmt *>(stmt);
  SqlResult     *sql_result      = session_event->sql_result();
  const char    *table_name      = show_index_stmt->table_name().c_str();

  Db    *db    = session->get_current_db();
  Table *table = db->find_table(table_name);
  // 下面展示的是table信息，现需要改为index信息 ！！！
  if (table != nullptr) {
    TupleSchema tuple_schema;
    tuple_schema.append_cell(TupleCellSpec("", "Field", "Field"));
    tuple_schema.append_cell(TupleCellSpec("", "Type", "Type"));
    tuple_schema.append_cell(TupleCellSpec("", "Length", "Length"));

    sql_result->set_tuple_schema(tuple_schema);

    auto             oper       = new StringListPhysicalOperator;
    const TableMeta &table_meta = table->table_meta();
    // sys_field_num()获取的是trx_fields_.size()
    for (int i = table_meta.sys_field_num(); i < table_meta.field_num(); i++) {
      const FieldMeta *field_meta = table_meta.field(i);
      oper->append({field_meta->name(), attr_type_to_string(field_meta->type()), to_string(field_meta->len())});
    }

    sql_result->set_operator(unique_ptr<PhysicalOperator>(oper));
  } else {

    sql_result->set_return_code(RC::SCHEMA_TABLE_NOT_EXIST);
    sql_result->set_state_string("Table not exists");
  }
  return rc;
}
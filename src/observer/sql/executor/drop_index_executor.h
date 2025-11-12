#pragma once

#include "common/sys/rc.h"

class SQLStageEvent;

/**
 * @brief 创建索引的执行器
 * @ingroup Executor
 * @note 删除索引
 */
class DropIndexExecutor
{
public:
  DropIndexExecutor()          = default;
  virtual ~DropIndexExecutor() = default;

  RC execute(SQLStageEvent *sql_event);
};
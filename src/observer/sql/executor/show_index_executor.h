#pragma once

#include "common/sys/rc.h"

class SQLStageEvent;

/**
 * @brief 描述表的执行器
 * @ingroup Executor
 */
class ShowIndexExecutor
{
public:
  ShowIndexExecutor()          = default;
  virtual ~ShowIndexExecutor() = default;

  RC execute(SQLStageEvent *sql_event);
};
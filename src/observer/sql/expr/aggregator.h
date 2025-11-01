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
// Created by Wangyunlai on 2024/05/29.
//

#pragma once

#include "common/value.h"
#include "common/sys/rc.h"

class Aggregator
{
public:
  virtual ~Aggregator() = default;

  virtual RC accumulate(const Value &value){return RC::UNIMPLEMENTED;};
  virtual RC average(const Value &value){return RC::UNIMPLEMENTED;};
  virtual RC count(const Value &value){return RC::UNIMPLEMENTED;};
  virtual RC max(const Value &value){return RC::UNIMPLEMENTED;};
  virtual RC min(const Value &value){return RC::UNIMPLEMENTED;};
  virtual RC evaluate(Value &result) = 0;

protected:
  Value value_;
};

class SumAggregator : public Aggregator
{
public:
  RC accumulate(const Value &value) override;
  RC evaluate(Value &result) override;
};

class AvgAggregator : public Aggregator
{
public:
  AvgAggregator() : count_(0) {}
  RC average(const Value &value) override;
  RC evaluate(Value &result) override;

private:
  int count_;  // 记录值的个数，用于计算平均值
};

class CountAggregator : public Aggregator
{
public:
  RC count(const Value &value) override;
  RC evaluate(Value &result) override;
};

class MaxAggregator : public Aggregator
{
public:
  RC max(const Value &value);
  RC evaluate(Value &result) override;
};

class MinAggregator : public Aggregator
{
public:
  RC min(const Value &value);
  RC evaluate(Value &result) override;
};
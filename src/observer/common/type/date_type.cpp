#include "common/lang/comparator.h"
#include "common/lang/sstream.h"
#include "common/lang/iomanip.h"
#include "common/log/log.h"
#include "common/type/date_type.h"
#include "common/value.h"
#include "common/time/datetime.h"
/**
 * 
 * @brief 日期类型
 * @ingroup DataType
 */

int DateType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::DATES && right.attr_type() == AttrType::DATES, "invalid type");
  // int i1 = left.value_.int_value_;
  // int i2 = right.value_.int_value_;
  // if (i1 > i2)
  // {
  //   return 1;
  // }
  // if (i1 < i2)
  // {
  //   return -1;
  // }
  // return 0;
  LOG_INFO("compare from DateType");
  return common::compare_int((void *)&left.value_.int_value_, (void *)&right.value_.int_value_);
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  int year, month, day;
  if (sscanf(data.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
    return RC::INVALID_DATE_FORMAT;
  }

  if (is_invalid_date(year, month, day)) {
    return RC::INVALID_DATE_FORMAT;
  }

  val.set_date(year, month, day);
  return RC::SUCCESS;
}

inline bool DateType::is_invalid_date(int year, int month, int day) const
{
  // 检查年份范围（通常数据库支持1900-9999年）
  if (year < 1900 || year > 9999) {
    return true;
  }

  // 检查月份范围
  if (month < 1 || month > 12) {
    return true;
  }

  // 检查日期范围
  if (day < 1) {
    return true;
  }

  // 获取指定月份的最大天数
  int max_days = 31;
  if (month == 4 || month == 6 || month == 9 || month == 11) {
    max_days = 30;
  } else if (month == 2) {
    // 2月特殊处理，考虑闰年
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
      max_days = 29;  // 闰年2月有29天
    } else {
      max_days = 28;  // 平年2月有28天
    }
  }

  // 检查日期是否超出该月的最大天数
  if (day > max_days) {
    return true;
  }

  return false;
}

RC DateType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    default: return RC::UNIMPLEMENTED;
  }
  return RC::SUCCESS;
}

int DateType::cast_cost(AttrType type)
{
  if (type == AttrType::DATES) {
    return 0;
  }
  return INT32_MAX;
}

RC DateType::to_string(const Value &val, string &result) const
{
  int year  = val.value_.int_value_ / 10000;
  int month = val.value_.int_value_ % 10000 / 100;
  int day   = val.value_.int_value_ % 100;
  stringstream ss;
  ss << setw(4) <<year << "-" << setfill('0') << setw(2) << month << "-" << setw(2) << day;
  result = ss.str();
  return RC::SUCCESS;
}
    
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
  return common::compare_int((void *)&left.value_.int_value_, (void *)&right.value_.int_value_);
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  int year, month, day;
  if (sscanf(data.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
    return RC::INVALID_DATE_FORMAT;
  }

  if (common::is_invalid_date(year, month, day)) {
    return RC::INVALID_DATE_FORMAT;
  }

  val.set_date(year, month, day);
  return RC::SUCCESS;
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
  int          year  = val.value_.int_value_ / 10000;
  int          month = val.value_.int_value_ % 10000 / 100;
  int          day   = val.value_.int_value_ % 100;
  stringstream ss;
  ss << year << "-" << setfill('0') << setw(2) << month << "-" << setw(2) << day;
  result = ss.str();
  return RC::SUCCESS;
}
    
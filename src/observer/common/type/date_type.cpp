#include "common/lang/comparator.h"
#include "common/log/log.h"
#include "common/type/date_type.h"
#include "common/value.h"
#include <iomanip>
#include <cassert>
/**
 * @brief 日期类型
 * @ingroup DataType
 */

int DateType::compare(const Value &left, const Value &right) const 
{
    ASSERT(left.attr_type() == AttrType::DATES && right.attr_type() == AttrType::DATES, "invalid type");
    return common::compare_int((void *)&left.value_.int_value_, (void *)&right.value_.int_value_);
}

RC DateType::cast_to(const Value &val, AttrType type, Value &result) const
{
    switch (type)
    {
        default: return RC::UNIMPLEMENTED;
    }
    return RC::SUCCESS;
}

RC DateType::set_value_from_str(Value &val, const string &data) const 
{
    int year, month, day;
    if (sscanf(data.c_str(), "%d-%d-%d",&year, &month, &day) != 3)
    {
        LOG_INFO("sscanf warning,year:%d month:%d day:%d",year,month,day);
        return RC::INVALID_DATE_FORMAT;
    }
    if (is_invalid_date(year,month,day))
    {
        LOG_INFO("date is invalid ,year:%d month:%d day:%d",year,month,day);
        return RC::INVALID_DATE_FORMAT;
    }
    val.set_date(year,month,day);
    return RC::SUCCESS;
}
    
bool DateType::is_invalid_date(int year,int month,int day)
{
    if (year < 1970 || year > 9999) // 年份上下限判断
    {
        return true;
    } 
    
    if(month < 1 || month > 12)
    {
        return true;
    }

    if (day <= 0 || day > 31)
    {
        return true;
    }
    else if (day == 31 && (month != 1 || month != 3 || month != 5 || month != 7 || month != 8 || month != 10 || month != 12))
    {
        return true;
    }
    else if (month == 2)
    {
        if (day > 29)
        {
            return true;
        }
        else if (day == 29)
        {
            if((year % 100 == 0 && year % 400 != 0) || (year % 4 != 0))
            {
                return true;
            }
        }
    }
    //闰年判断

    return false;
}

int DateType::cast_cost(AttrType type)
{
    if (type == AttrType::DATES)
    {
        return 0;
    }
    return INT32_MAX;
}

RC DateType::to_string(const Value &val,string &result) const 
{
    int year = val.value_.int_value_ /10000;
    int month = val.value_.int_value_ % 10000/100;
    int day = val.value_.int_value_ % 100;
    stringstream ss;
    ss << year << "-" << std::setfill('0') << std::setw(2) << month << "-" << std::setw(2) << day;
    result = ss.str();
    return RC::SUCCESS;
}

#pragma once

#include <cstdint>

namespace services::water {

/**
 * Water intake log, kept in NVS (survives power loss).
 *
 *  - goal: daily target in ml, set on the device, kept until changed
 *  - hourly amounts for the last kHourlyDays days (24 values per day)
 *  - daily totals for the last kTotalDays days
 *
 * Days are numbered as days since 1970-01-01 in local time (see dayNumber()).
 */
constexpr int kHourlyDays = 21;
constexpr int kTotalDays = 366;
constexpr uint16_t kDefaultGoalMl = 2000;

void init();

uint16_t goalMl();
void setGoalMl(uint16_t ml);

/** Local calendar date -> day number. */
int32_t dayNumber(int year, int month /*1-12*/, int mday);
/** Day number -> date. wday: 0 = Sunday. */
void dateOf(int32_t day, int* year, int* month, int* mday, int* wday);
/** Today's day number and the current hour; false until the clock is set. */
bool today(int32_t* day, int* hour);

/** Add a drink to the current hour of today. False if the clock is not set. */
bool add(uint16_t ml);
/** Remove the last drink added since power-up. False if there is nothing to undo. */
bool undoLast();

/** Hourly amounts of a day; false if no hourly data is kept for it (all zero). */
bool hours(int32_t day, uint16_t out[24]);
/** Total of a day in ml (0 if nothing was logged or it is too old). */
uint16_t total(int32_t day);

}  // namespace services::water

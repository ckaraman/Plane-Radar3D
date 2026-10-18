#include "services/water_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>
#include <ctime>

#include "services/clock_service.h"

namespace services::water {

namespace {

constexpr char kNs[] = "water";

struct HourRec {
  int32_t day;      // -1 = empty slot
  uint16_t ml[24];
};
struct TotalRec {
  int32_t day;
  uint16_t ml;
  uint16_t pad;
};

uint16_t s_goal = kDefaultGoalMl;
HourRec s_hours[kHourlyDays];
TotalRec s_totals[kTotalDays];
bool s_loaded = false;

// Last drink (for undo), RAM only.
int32_t s_undo_day = -1;
int s_undo_hour = -1;
uint16_t s_undo_ml = 0;

void save() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putBytes("hours", s_hours, sizeof(s_hours));
  p.putBytes("tot", s_totals, sizeof(s_totals));
  p.end();
}

HourRec* hourRec(int32_t day, bool create) {
  HourRec& r = s_hours[((day % kHourlyDays) + kHourlyDays) % kHourlyDays];
  if (r.day == day) return &r;
  if (!create) return nullptr;
  r.day = day;
  memset(r.ml, 0, sizeof(r.ml));
  return &r;
}

TotalRec* totalRec(int32_t day, bool create) {
  TotalRec& r = s_totals[((day % kTotalDays) + kTotalDays) % kTotalDays];
  if (r.day == day) return &r;
  if (!create) return nullptr;
  r.day = day;
  r.ml = 0;
  return &r;
}

void change(int32_t day, int hour, int delta) {
  HourRec* h = hourRec(day, true);
  TotalRec* t = totalRec(day, true);
  const int hv = static_cast<int>(h->ml[hour]) + delta;
  const int tv = static_cast<int>(t->ml) + delta;
  h->ml[hour] = static_cast<uint16_t>(hv < 0 ? 0 : (hv > 65000 ? 65000 : hv));
  t->ml = static_cast<uint16_t>(tv < 0 ? 0 : (tv > 65000 ? 65000 : tv));
  save();
}

}  // namespace

void init() {
  for (auto& r : s_hours) r.day = -1;
  for (auto& r : s_totals) r.day = -1;
  Preferences p;
  if (p.begin(kNs, true)) {
    s_goal = p.getUShort("goal", kDefaultGoalMl);
    if (p.getBytesLength("hours") == sizeof(s_hours)) p.getBytes("hours", s_hours, sizeof(s_hours));
    if (p.getBytesLength("tot") == sizeof(s_totals)) p.getBytes("tot", s_totals, sizeof(s_totals));
    p.end();
  }
  s_loaded = true;
}

uint16_t goalMl() { return s_goal; }

void setGoalMl(uint16_t ml) {
  s_goal = ml;
  Preferences p;
  if (p.begin(kNs, false)) {
    p.putUShort("goal", ml);
    p.end();
  }
}

int32_t dayNumber(int y, int m, int d) {
  // Howard Hinnant's days_from_civil.
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

void dateOf(int32_t z, int* year, int* month, int* mday, int* wday) {
  if (wday) *wday = static_cast<int>(((z % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  if (year) *year = static_cast<int>(yoe) + era * 400 + (m <= 2);
  if (month) *month = m;
  if (mday) *mday = d;
}

bool today(int32_t* day, int* hour) {
  tm t;
  if (!services::clock::now(&t) || t.tm_year < 120) return false;
  if (day) *day = dayNumber(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  if (hour) *hour = t.tm_hour;
  return true;
}

bool add(uint16_t ml) {
  int32_t day;
  int hour;
  if (!today(&day, &hour)) return false;
  change(day, hour, ml);
  s_undo_day = day;
  s_undo_hour = hour;
  s_undo_ml = ml;
  return true;
}

bool undoLast() {
  if (s_undo_ml == 0 || s_undo_day < 0) return false;
  change(s_undo_day, s_undo_hour, -static_cast<int>(s_undo_ml));
  s_undo_ml = 0;
  return true;
}

bool hours(int32_t day, uint16_t out[24]) {
  const HourRec* r = hourRec(day, false);
  if (r == nullptr) {
    memset(out, 0, sizeof(uint16_t) * 24);
    return false;
  }
  memcpy(out, r->ml, sizeof(r->ml));
  return true;
}

uint16_t total(int32_t day) {
  const TotalRec* r = totalRec(day, false);
  return r ? r->ml : 0;
}

}  // namespace services::water

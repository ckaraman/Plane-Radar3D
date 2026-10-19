#include "services/water_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "services/clock_service.h"

namespace services::water {

namespace {

constexpr char kNs[] = "water";
constexpr char kKeyHours[] = "h7";   // HourRec[kHourlyDays]
constexpr char kKeyTotals[] = "t90"; // Totals

// The data lives in NVS (flash); RAM keeps this compact copy so the screen can
// read it on every redraw without touching flash: ~550 bytes in all.
struct HourRec {
  int32_t day;  // -1 = empty slot
  uint16_t ml[24];
};
struct Totals {
  int32_t newest;              // day number of ml[0]; -1 = nothing recorded yet
  uint16_t ml[kTotalDays];     // ml[i] = total of day (newest - i)
};

uint16_t s_goal = kDefaultGoalMl;
HourRec s_hours[kHourlyDays];
Totals s_tot;

// Last drink (for undo), RAM only.
int32_t s_undo_day = -1;
int s_undo_hour = -1;
uint16_t s_undo_ml = 0;

void save() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putBytes(kKeyHours, s_hours, sizeof(s_hours));
  p.putBytes(kKeyTotals, &s_tot, sizeof(s_tot));
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

/** Slot for `day` in the totals window (moving the window forward for a new day). */
uint16_t* totalSlot(int32_t day, bool create) {
  if (s_tot.newest < 0) {
    if (!create) return nullptr;
    s_tot.newest = day;
    memset(s_tot.ml, 0, sizeof(s_tot.ml));
  }
  if (day > s_tot.newest) {
    if (!create) return nullptr;
    const int32_t shift = day - s_tot.newest;
    if (shift >= kTotalDays) {
      memset(s_tot.ml, 0, sizeof(s_tot.ml));
    } else {
      memmove(s_tot.ml + shift, s_tot.ml, sizeof(uint16_t) * (kTotalDays - shift));
      memset(s_tot.ml, 0, sizeof(uint16_t) * shift);
    }
    s_tot.newest = day;
  }
  const int32_t idx = s_tot.newest - day;
  if (idx < 0 || idx >= kTotalDays) return nullptr;  // older than the window
  return &s_tot.ml[idx];
}

void change(int32_t day, int hour, int delta) {
  HourRec* h = hourRec(day, true);
  const int hv = static_cast<int>(h->ml[hour]) + delta;
  h->ml[hour] = static_cast<uint16_t>(hv < 0 ? 0 : (hv > 65000 ? 65000 : hv));
  if (uint16_t* t = totalSlot(day, true)) {
    const int tv = static_cast<int>(*t) + delta;
    *t = static_cast<uint16_t>(tv < 0 ? 0 : (tv > 65000 ? 65000 : tv));
  }
  save();
}

/**
 * One-time move from the first layout (21 days hourly + 366 days of totals, 4 KB
 * kept in RAM): keep the latest 7 days hourly and 90 days of totals, then drop
 * the old keys.
 */
void migrateOld(Preferences& p) {
  struct OldTotal {
    int32_t day;
    uint16_t ml;
    uint16_t pad;
  };
  constexpr size_t kOldHours = 21, kOldTotals = 366;
  bool changed = false;
  if (p.getBytesLength("hours") == sizeof(HourRec) * kOldHours) {
    auto* old = static_cast<HourRec*>(malloc(sizeof(HourRec) * kOldHours));
    if (old != nullptr) {
      p.getBytes("hours", old, sizeof(HourRec) * kOldHours);
      for (size_t i = 0; i < kOldHours; ++i) {
        if (old[i].day < 0) continue;
        HourRec& slot = s_hours[((old[i].day % kHourlyDays) + kHourlyDays) % kHourlyDays];
        if (old[i].day > slot.day) slot = old[i];  // the newest day wins the slot
      }
      free(old);
      changed = true;
    }
  }
  if (p.getBytesLength("tot") == sizeof(OldTotal) * kOldTotals) {
    auto* old = static_cast<OldTotal*>(malloc(sizeof(OldTotal) * kOldTotals));
    if (old != nullptr) {
      p.getBytes("tot", old, sizeof(OldTotal) * kOldTotals);
      int32_t newest = -1;
      for (size_t i = 0; i < kOldTotals; ++i) newest = old[i].day > newest ? old[i].day : newest;
      if (newest >= 0) {
        s_tot.newest = newest;
        memset(s_tot.ml, 0, sizeof(s_tot.ml));
        for (size_t i = 0; i < kOldTotals; ++i) {
          const int32_t idx = newest - old[i].day;
          if (old[i].day >= 0 && idx >= 0 && idx < kTotalDays) s_tot.ml[idx] = old[i].ml;
        }
      }
      free(old);
      changed = true;
    }
  }
  if (changed) {
    p.putBytes(kKeyHours, s_hours, sizeof(s_hours));
    p.putBytes(kKeyTotals, &s_tot, sizeof(s_tot));
    p.remove("hours");
    p.remove("tot");
    Serial.println("water: history moved to the 90-day layout");
  }
}

}  // namespace

void init() {
  for (auto& r : s_hours) r.day = -1;
  s_tot.newest = -1;
  memset(s_tot.ml, 0, sizeof(s_tot.ml));
  Preferences p;
  if (!p.begin(kNs, false)) return;
  s_goal = p.getUShort("goal", kDefaultGoalMl);
  if (p.getBytesLength(kKeyHours) == sizeof(s_hours)) p.getBytes(kKeyHours, s_hours, sizeof(s_hours));
  if (p.getBytesLength(kKeyTotals) == sizeof(s_tot)) {
    p.getBytes(kKeyTotals, &s_tot, sizeof(s_tot));
  } else {
    migrateOld(p);
  }
  p.end();
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
  const uint16_t* t = totalSlot(day, false);
  return t ? *t : 0;
}

}  // namespace services::water

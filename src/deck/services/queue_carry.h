#pragma once

#include <cstdint>

namespace services::host {

/** A new queue keeps the row pictures the card already has (HELLO "qkeep", stage 10b-3):
 *  new row k takes old row from[k]'s picture (art, art_len), which then has none. A from
 *  out of range, -1 or naming a row with no picture (left, or taken already) does nothing. */
template <typename Row>
void carryArts(Row* old_rows, int old_n, Row* new_rows, int new_n, const int16_t* from) {
  for (int k = 0; k < new_n; ++k) {
    const int j = from[k];
    if (j < 0 || j >= old_n || old_rows[j].art == nullptr) continue;
    new_rows[k].art = old_rows[j].art;
    new_rows[k].art_len = old_rows[j].art_len;
    old_rows[j].art = nullptr;
    old_rows[j].art_len = 0;
  }
}

}  // namespace services::host

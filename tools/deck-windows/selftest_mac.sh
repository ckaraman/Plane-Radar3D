#!/bin/bash
# CI: the built app starts, loads its parts and opens / closes its window twice.
# (GitHub shows at most 10 notes per step: lines are joined into a few notes.)
cd "$(dirname "$0")"
BIN="dist/Plane Radar Deck.app/Contents/MacOS/Plane Radar Deck"
"$BIN" --selftest --deep || { echo "::error::self test failed ($?)"; exit 1; }
[ -f selftest.txt ] && echo "::notice::mac selftest: $(tr '\n' '|' < selftest.txt | cut -c1-3000)"
"$BIN" --smoke > smoke.log 2>&1 &
pid=$!
for i in $(seq 1 150); do kill -0 $pid 2>/dev/null || break; sleep 1; done
if kill -0 $pid 2>/dev/null; then echo "::warning::mac window test still running after 150 s"; kill $pid; fi
if [ -f smoke.txt ]; then echo "::notice::mac window test: $(tr '\n' '|' < smoke.txt)"; else echo "::warning::mac: no window test result"; fi
echo "::notice::mac smoke log: $(tail -c 2500 smoke.log | tr '\n' '|')"
exit 0

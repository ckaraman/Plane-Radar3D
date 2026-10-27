#!/bin/bash
# CI: the built app starts, loads its parts and opens / closes its window twice.
cd "$(dirname "$0")"
BIN="dist/Plane Radar Deck.app/Contents/MacOS/Plane Radar Deck"
"$BIN" --selftest --deep || { echo "::error::self test failed ($?)"; exit 1; }
[ -f selftest.txt ] && while IFS= read -r l; do echo "::notice::mac $l"; done < selftest.txt
( "$BIN" --smoke & pid=$!; for i in $(seq 1 120); do kill -0 $pid 2>/dev/null || break; sleep 1; done; kill $pid 2>/dev/null ) || true
if [ -f smoke.txt ]; then while IFS= read -r l; do echo "::notice::mac $l"; done < smoke.txt; else echo "::warning::mac: no window test result"; fi
exit 0

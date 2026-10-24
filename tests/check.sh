#!/usr/bin/env bash
# Runs qmlcoverage over the fixtures and checks exact lcov records.
# Usage: tests/check.sh <build-dir>
set -u

build=$(realpath "$1")
here=$(cd "$(dirname "$0")" && pwd)
runner=${QMLTESTRUNNER:-/usr/lib/qt6/bin/qmltestrunner}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export QT_QPA_PLATFORM=offscreen
failures=0

fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# expect <file> <record>: the record appears in that file's lcov section.
expect() {
    awk -v sf="SF:$1" -v rec="$2" '$0 == sf {in_sf = 1; next} /^end_of_record/ {in_sf = 0}
        in_sf && $0 == rec {found = 1} END {exit !found}' "$3" || fail "$1: missing '$2'"
}

absent() {
    awk -v sf="SF:$1" -v rec="$2" '$0 == sf {in_sf = 1; next} /^end_of_record/ {in_sf = 0}
        in_sf && index($0, rec) == 1 {found = 1} END {exit found}' "$3" || fail "$1: unexpected '$2'"
}

run() { # run <out.info> <dir> [--plugins]
    local out=$1 dir=$2; shift 2
    "$build/qmlcoverage" "$@" -o "$out" "$runner" -input "$dir" >"$out.log" 2>&1
}

# Red: without the plugin there is no report, and that must not look like success.
run "$work/red.info" "$here/fixture"
[ $? -eq 4 ] || fail "missing plugin: expected exit 4"
[ ! -e "$work/red.info" ] || fail "missing plugin: an lcov file was written"

# Refused: a plugin built for an older or a newer Qt must say so and must not report.
run "$work/older.info" "$here/fixture" --plugins "$build/olderqt/plugins"
[ $? -eq 4 ] || fail "plugin for an older Qt: expected exit 4"
grep -q "CodeCoverage: built for Qt 0.0.0 but running on Qt" "$work/older.info.log" \
    || fail "plugin for an older Qt: no warning"
run "$work/newer.info" "$here/fixture" --plugins "$build/newerqt/plugins"
[ $? -eq 4 ] || fail "plugin for a newer Qt: expected exit 4"
grep -q "CodeCoverage: built for Qt 99.0.0 but running on Qt" "$work/newer.info.log" \
    || fail "plugin for a newer Qt: no warning"

# Unwritable: a report or source copy that cannot be written is an error, not a success.
# The report is small, so it fails only once the file's own buffer is flushed.
QT_QPA_PLATFORM=offscreen python3 -I "$here/interrupt.py" 1.5 "$build/qmlcoverage" \
    --plugins "$build/plugins" -o /dev/full --sources "$work/full.src" "$build/twoapp" \
    "$here/twoapp/EngineA.qml" "$here/twoapp/EngineB.qml" >"$work/full.log" 2>&1
grep -qx "exit=2" "$work/full.log" || fail "full disk: expected exit 2"
grep -q "cannot write /dev/full" "$work/full.log" || fail "full disk: no message"
run "$work/nosrc.info" "$here/fixture" --plugins "$build/plugins" --sources /dev/null/src
[ $? -eq 2 ] || fail "unwritable sources: expected exit 2"
grep -q "cannot write /dev/null/src/qrc/" "$work/nosrc.info.log" || fail "unwritable sources: no message"

# One engine. The output exists already and is longer than the report: it must be replaced.
calc=$here/fixture/Calc.qml
util=$here/fixture/util.js
first=$here/fixture/first.js
tst=$here/fixture/tst_calc.qml
one=$work/one.info
head -c 200000 /dev/zero | tr '\0' 'x' >"$one"
run "$one" "$here/fixture" --plugins "$build/plugins" || fail "fixture run failed"
! grep -q "xxxx" "$one" || fail "old output not truncated"
expect "$calc" "FNDA:4,add" "$one"              # 1 direct call + 3 from the loop
expect "$calc" "FNDA:0,neverCalled" "$one"
expect "$calc" "FNDA:1,expression for doubled" "$one"
expect "$calc" "DA:6,4" "$one"                  # one-line function: once per call, not per table entry
expect "$calc" "DA:7,0" "$one"
expect "$calc" "DA:11,3" "$one"                 # loop body
expect "$calc" "DA:12,1" "$one"
absent "$calc" "DA:13," "$one"                  # closing brace after the final return
expect "$util" "FNDA:1,square" "$one"
expect "$util" "FNDA:0,unusedJs" "$one"
expect "$util" "DA:8,0" "$one"
absent "$util" "DA:5," "$one"
absent "$util" "DA:9," "$one"                   # file's last brace: root epilogue
expect "$util" "DA:3,2" "$one"                  # %entry and square both start on line 3
expect "$calc" "LF:9" "$one"
expect "$calc" "LH:8" "$one"
expect "$calc" "FNF:5" "$one"
expect "$calc" "FNH:4" "$one"
expect "$first" "DA:1,2" "$one"                 # code on the first line
expect "$first" "FNDA:1,first" "$one"
expect "$tst" "FNDA:1,<anonymous>:10" "$one"

# Two engines (one per test file): counts are summed per file.
cp "$here/fixture/"* "$work/"
cp "$here/twoengines/"* "$work/"
run "$work/two.info" "$work" --plugins "$build/plugins" || fail "two-engine run failed"
grep -q "2 report(s)" "$work/two.info.log" || fail "two engines: expected 2 reports"
expect "$work/Calc.qml" "FNDA:1,neverCalled" "$work/two.info"
expect "$work/Calc.qml" "DA:7,1" "$work/two.info"
expect "$work/Calc.qml" "FNDA:4,add" "$work/two.info"
expect "$work/Calc.qml" "DA:6,4" "$work/two.info"    # lines summed too, not last report wins

# Two engines alive at once, stopped with Ctrl+C: each answers the snapshot, then quits cleanly
# and sends a final report that holds only what ran since the snapshot.
QT_QPA_PLATFORM=offscreen python3 -I "$here/interrupt.py" 1.5 "$build/qmlcoverage" \
    --plugins "$build/plugins" -o "$work/concurrent.info" "$build/twoapp" \
    "$here/twoapp/EngineA.qml" "$here/twoapp/EngineB.qml" >"$work/concurrent.log" 2>&1
grep -qx "exit=0" "$work/concurrent.log" || fail "concurrent engines: expected exit 0"
grep -q "4 report(s)" "$work/concurrent.log" || fail "concurrent engines: expected 2 snapshot + 2 final reports"
expect "$here/twoapp/EngineA.qml" "FNDA:1,once" "$work/concurrent.info"
expect "$here/twoapp/EngineB.qml" "FNDA:1,once" "$work/concurrent.info"
grep -qE "^FNDA:[1-9][0-9]*,tick$" "$work/concurrent.info" || fail "concurrent engines: tick not hit"

# twoapp run stopped with Ctrl+C after 1.5 s: twoapp_run <name> [twoapp options] <a.qml> <b.qml>
twoapp_run() {
    local name=$1; shift
    QT_QPA_PLATFORM=offscreen python3 -I "$here/interrupt.py" 1.5 "$build/qmlcoverage" \
        --plugins "$build/plugins" -o "$work/$name.info" "$build/twoapp" "$@" >"$work/$name.log" 2>&1
}

# Every engine already gone when the snapshot is asked for: answered at once, with nothing.
twoapp_run released --release "$here/twoapp/EngineA.qml" "$here/twoapp/EngineB.qml"
grep -qx "exit=0" "$work/released.log" || fail "released: expected exit 0"
grep -q "3 report(s)" "$work/released.log" || fail "released: expected 2 final reports + 1 empty"
expect "$here/twoapp/EngineA.qml" "FNDA:1,once" "$work/released.info"

# A file with no source to point at (app:/ sorts before file:) is skipped; the others are written.
twoapp_run inline --inline "$here/twoapp/EngineA.qml" "$here/twoapp/EngineB.qml"
grep -qx "exit=0" "$work/inline.log" || fail "inline: expected exit 0"
grep -q "no source for app:/EngineB.qml; not written" "$work/inline.log" || fail "inline: no skip message"
expect "$here/twoapp/EngineA.qml" "FNDA:1,once" "$work/inline.info"
[ "$(grep -c '^SF:' "$work/inline.info")" -eq 1 ] || fail "inline: the skipped file has a record"

# Control flow: branches(1), branches(-1), branches(-2); loops(3); sw(1), sw(2), sw(3);
# guarded() once returning, once throwing.
run "$work/flow.info" "$here/controlflow" --plugins "$build/plugins" || fail "control-flow run failed"
ctl=$here/controlflow/Ctl.qml
flow=$work/flow.info
expect "$ctl" "DA:6,1" "$flow"    # then
expect "$ctl" "DA:8,2" "$flow"    # else
expect "$ctl" "DA:14,3" "$flow"   # for body
expect "$ctl" "DA:16,4" "$flow"   # while condition
expect "$ctl" "DA:17,3" "$flow"   # while body
expect "$ctl" "DA:19,1" "$flow"   # do body
expect "$ctl" "DA:26,1" "$flow"   # case 1
expect "$ctl" "DA:28,2" "$flow"   # default
expect "$ctl" "DA:35,1" "$flow"   # try: return after the call that did not throw
expect "$ctl" "DA:37,1" "$flow"   # catch
expect "$ctl" "DA:39,2" "$flow"   # finally
# A for header line is entered once however often its test and update run (no Debug
# instruction marks the jump back to them). Only its coverage is meaningful.
grep -qE "^DA:13,[1-9]" "$flow" || fail "$ctl: for header not covered"

# qrc: sources come from the program and are written next to the report.
testcase=$work/one.info.src/qrc/qt-project.org/imports/QtTest/TestCase.qml
[ -s "$testcase" ] || fail "qrc: TestCase.qml source not written"
grep -qx "SF:$testcase" "$one" || fail "qrc: no record for TestCase.qml"
# TestCase.qml ships AOT-compiled in libQt6QuickTest; debug mode must still route it through us.
awk -v sf="SF:$testcase" '$0 == sf {in_sf = 1; next} /^end_of_record/ {in_sf = 0}
    in_sf && /^FNDA:[1-9]/ {hit = 1} END {exit !hit}' "$one" || fail "qrc: no TestCase.qml hits"

# A program that never exits: Ctrl+C snapshots every engine, stops it and writes the report.
QT_QPA_PLATFORM=offscreen python3 -I "$here/interrupt.py" 2 "$build/qmlcoverage" \
    --plugins "$build/plugins" -o "$work/forever.info" "${QML:-/usr/lib/qt6/bin/qml}" \
    "$here/longrunning/forever.qml" >"$work/forever.log" 2>&1
grep -qx "exit=0" "$work/forever.log" || fail "interrupt: expected exit 0"
forever=$here/longrunning/forever.qml
expect "$forever" "FNDA:0,never" "$work/forever.info"
grep -qE "^FNDA:[1-9][0-9]*,tick$" "$work/forever.info" || fail "interrupt: tick not hit"

# Format gate: lcov must read the file without messages.
if command -v lcov >/dev/null; then
    lcov --summary "$one" >"$work/lcov.log" 2>&1 || fail "lcov rejected the tracefile"
    grep -q "no messages were reported" "$work/lcov.log" || fail "lcov reported messages"
else
    fail "lcov not installed; format unchecked"
fi

[ "$failures" -eq 0 ] && echo "PASS" || echo "$failures failure(s)"
exit "$failures"

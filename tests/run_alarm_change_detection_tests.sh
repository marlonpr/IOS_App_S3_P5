#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/alarm-change-tests.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CXX="${CXX:-c++}"

"$CXX" \
    -std=c++17 \
    -DCLOCK_ALARM_ENABLE_TEST_HOOKS \
    -I"$REPO_ROOT/tests/stubs" \
    -I"$REPO_ROOT/main" \
    "$REPO_ROOT/tests/alarm_change_detection_test.cpp" \
    "$REPO_ROOT/tests/alarm_test_stubs.cpp" \
    "$REPO_ROOT/main/clock_alarm.cpp" \
    "$REPO_ROOT/main/clock_protocol.cpp" \
    -o "$BUILD_DIR/alarm_change_detection_test"

"$BUILD_DIR/alarm_change_detection_test"

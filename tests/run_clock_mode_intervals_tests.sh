#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/clock-interval-tests.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CXX="${CXX:-c++}"

"$CXX" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -I"$REPO_ROOT/main" \
    -I"$REPO_ROOT/tests/stubs" \
    "$REPO_ROOT/tests/clock_mode_intervals_test.cpp" \
    "$REPO_ROOT/tests/interval_settings_test_stubs.cpp" \
    "$REPO_ROOT/main/clock_modes.cpp" \
    "$REPO_ROOT/main/clock_settings.cpp" \
    -o "$BUILD_DIR/clock_mode_intervals_test"

"$BUILD_DIR/clock_mode_intervals_test"

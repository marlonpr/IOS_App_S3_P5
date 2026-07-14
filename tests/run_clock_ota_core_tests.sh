#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/clock-ota-tests.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CXX="${CXX:-c++}"

"$CXX" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$REPO_ROOT/main" \
    "$REPO_ROOT/tests/clock_ota_core_test.cpp" \
    "$REPO_ROOT/main/clock_ota_core.cpp" \
    -o "$BUILD_DIR/clock_ota_core_test"

"$BUILD_DIR/clock_ota_core_test"

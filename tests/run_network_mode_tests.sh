#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/network-mode-tests.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CXX="${CXX:-c++}"

"$CXX" \
    -std=c++17 \
    -I"$REPO_ROOT/tests/stubs" \
    -I"$REPO_ROOT/main" \
    "$REPO_ROOT/tests/network_mode_test.cpp" \
    "$REPO_ROOT/tests/network_settings_test_stubs.cpp" \
    "$REPO_ROOT/main/clock_network_core.cpp" \
    "$REPO_ROOT/main/clock_settings.cpp" \
    -o "$BUILD_DIR/network_mode_test"

"$BUILD_DIR/network_mode_test"

#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/network-status-tests.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CXX="${CXX:-c++}"

"$CXX" \
    -std=c++17 \
    -I"$REPO_ROOT/tests/stubs" \
    -I"$REPO_ROOT/main" \
    "$REPO_ROOT/tests/network_status_test.cpp" \
    "$REPO_ROOT/main/clock_network_core.cpp" \
    "$REPO_ROOT/main/clock_network_indicator.cpp" \
    -o "$BUILD_DIR/network_status_test"

"$BUILD_DIR/network_status_test"

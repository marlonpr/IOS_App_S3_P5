#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/palette-protocol-tests.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CXX="${CXX:-c++}"

"$CXX" \
    -std=c++17 \
    -I"$REPO_ROOT/tests/stubs" \
    -I"$REPO_ROOT/main" \
    "$REPO_ROOT/tests/palette_protocol_test.cpp" \
    "$REPO_ROOT/tests/protocol_test_stubs.cpp" \
    "$REPO_ROOT/tests/palette_test_stubs.cpp" \
    "$REPO_ROOT/main/clock_protocol.cpp" \
    "$REPO_ROOT/main/clock_protocol_stream.cpp" \
    "$REPO_ROOT/main/clock_palette.cpp" \
    -o "$BUILD_DIR/palette_protocol_test"

"$BUILD_DIR/palette_protocol_test"

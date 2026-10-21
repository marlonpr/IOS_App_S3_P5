#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/logo-sequence-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

"${CXX:-c++}" -std=c++17 \
    -I"$REPO_ROOT/main" \
    -I"$REPO_ROOT/tests/stubs" \
    -I"$REPO_ROOT/components/led_panel" \
    "$REPO_ROOT/tests/logo_sequence_test.cpp" \
    -o "$BUILD_DIR/logo_sequence_test"

"$BUILD_DIR/logo_sequence_test"

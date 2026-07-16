#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/button-logic-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

"${CXX:-c++}" -std=c++17 -I"$REPO_ROOT/tests/stubs" -I"$REPO_ROOT/main" \
    "$REPO_ROOT/tests/button_logic_test.cpp" \
    "$REPO_ROOT/main/clock_button_logic.cpp" \
    "$REPO_ROOT/main/clock_menu_model.cpp" \
    -o "$BUILD_DIR/button_logic_test"

"$BUILD_DIR/button_logic_test"

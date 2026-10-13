#!/usr/bin/env bash
# Builds and runs the whole HB Download Manager test suite.
#
#   ./run_tests.sh              build with qmake + make, run every test
#   QMAKE=/path/to/qmake6 ./run_tests.sh
#   SKIP_BUILD=1 ./run_tests.sh   run the already built binaries only
#   SKIP_NODE=1 ./run_tests.sh    do not require node for the extension syntax check
#
# Exit status is non-zero when the build fails, a test binary is missing, any test fails, or a
# safety check fails. "ALL TESTS PASSED" is printed only when everything passed.
set -u
set -o pipefail
export QT_QPA_PLATFORM=offscreen

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "====================================================="
echo "       HB Download Manager - Automated Test Runner    "
echo "====================================================="

FAILURES=()
fail() {
    echo "FAIL: $1" >&2
    FAILURES+=("$1")
}

# --- safety guard: the tests must never touch the real configuration -------------------------
REAL_DIR="$HOME/.config/hb-download-manager"
REAL_FILES=("$REAL_DIR/downloads.json" "$REAL_DIR/bridge-token")
declare -A BEFORE
for f in "${REAL_FILES[@]}"; do
    if [ -f "$f" ]; then
        BEFORE["$f"]=$(md5sum "$f" | awk '{print $1}')
    else
        BEFORE["$f"]="absent"
    fi
done

# --- build ----------------------------------------------------------------------------------
if [ -z "${SKIP_BUILD:-}" ]; then
    QMAKE_BIN="${QMAKE:-}"
    if [ -z "$QMAKE_BIN" ]; then
        for candidate in qmake6 qmake "$HOME/Qt/6.11.1/gcc_64/bin/qmake"; do
            if [ -x "$candidate" ] || command -v "$candidate" >/dev/null 2>&1; then
                QMAKE_BIN="$candidate"
                break
            fi
        done
    fi
    if [ -z "$QMAKE_BIN" ]; then
        echo "ERROR: no qmake found (install Qt, or set QMAKE=/path/to/qmake)." >&2
        exit 1
    fi
    echo "Using qmake: $QMAKE_BIN"

    if ! ( cd tests && "$QMAKE_BIN" tests.pro ); then
        echo "ERROR: qmake failed." >&2
        exit 1
    fi
    echo "Building test binaries..."
    if ! ( cd tests && make -j"$(nproc)" ); then
        echo "ERROR: building the tests failed." >&2
        exit 1
    fi
fi

# --- run every test -------------------------------------------------------------------------
TEST_TARGETS=(
    test_rangeplanner
    test_range_validation
    test_pathutils
    test_json_roundtrip
    test_bridge_auth
    test_download_lifecycle
    test_engine_integration
)

TIMEOUT_CMD=()
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(timeout 900)
fi

echo ""
echo "Executing test suites..."
cd tests
for t in "${TEST_TARGETS[@]}"; do
    echo "-----------------------------------------------------"
    if [ ! -x "./$t" ]; then
        fail "$t: binary not found or not executable"
        continue
    fi
    echo "Running ./$t ..."
    if ${TIMEOUT_CMD[@]+"${TIMEOUT_CMD[@]}"} "./$t"; then
        echo "OK: $t"
    else
        fail "$t: tests failed (exit $?)"
    fi
done
cd "$ROOT"

# --- test server self-test --------------------------------------------------------------------
echo "-----------------------------------------------------"
echo "Running tests/test_server.py --self-test ..."
if ! python3 tests/test_server.py --self-test; then
    fail "test_server.py --self-test"
fi

# --- extension syntax check ---------------------------------------------------------------------
echo "-----------------------------------------------------"
if command -v node >/dev/null 2>&1; then
    echo "Checking extension JavaScript (node --check) ..."
    for js in extension/*.js; do
        if ! node --check "$js"; then
            fail "node --check $js"
        fi
    done
elif [ -n "${SKIP_NODE:-}" ]; then
    echo "SKIP_NODE set: not checking extension JavaScript."
else
    fail "node is not installed (set SKIP_NODE=1 to skip the extension syntax check)"
fi

# --- version consistency ------------------------------------------------------------------------
echo "-----------------------------------------------------"
if ! bash scripts/check_version.sh; then
    fail "scripts/check_version.sh"
fi

# --- safety guard result ---------------------------------------------------------------------------
echo "-----------------------------------------------------"
GUARD_OK=1
for f in "${REAL_FILES[@]}"; do
    if [ -f "$f" ]; then
        after=$(md5sum "$f" | awk '{print $1}')
    else
        after="absent"
    fi
    if [ "${BEFORE[$f]}" != "$after" ]; then
        echo "CRITICAL SAFETY VIOLATION: $f was changed by the test suite!" >&2
        GUARD_OK=0
    fi
done
if [ "$GUARD_OK" -eq 1 ]; then
    echo "Test mode verification passed: the real downloads.json and bridge-token were NOT modified."
else
    fail "real configuration was modified"
fi

echo ""
echo "====================================================="
if [ "${#FAILURES[@]}" -eq 0 ]; then
    echo "       ALL TESTS PASSED                               "
    echo "====================================================="
    exit 0
fi
echo "       TESTS FAILED (${#FAILURES[@]})                 "
for item in "${FAILURES[@]}"; do
    echo "  - $item"
done
echo "====================================================="
exit 1

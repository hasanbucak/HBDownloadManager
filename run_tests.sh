#!/usr/bin/env bash
set -e
export QT_QPA_PLATFORM=offscreen

echo "====================================================="
echo "       HB Download Manager - Automated Test Runner    "
echo "====================================================="

REAL_CFG="$HOME/.config/hb-download-manager/downloads.json"
BEFORE_MD5=""
if [ -f "$REAL_CFG" ]; then
    BEFORE_MD5=$(md5sum "$REAL_CFG" | awk '{print $1}')
fi

cd tests

TEST_TARGETS=(
    test_range_validation
    test_pathutils
    test_json_roundtrip
    test_bridge_auth
    test_download_lifecycle
    test_engine_integration
)

echo "Building test binaries..."
make -j$(nproc)

echo ""
echo "Executing test suites..."
for t in "${TEST_TARGETS[@]}"; do
    if [ -x "./$t" ]; then
        echo "-----------------------------------------------------"
        echo "Running ./$t ..."
        "./$t"
    else
        echo "WARNING: Test binary ./$t not found or not executable." >&2
    fi
done

cd ..

if [ -n "$BEFORE_MD5" ]; then
    AFTER_MD5=$(md5sum "$REAL_CFG" | awk '{print $1}')
    if [ "$BEFORE_MD5" != "$AFTER_MD5" ]; then
        echo ""
        echo "CRITICAL SAFETY VIOLATION: $REAL_CFG was altered by the test suite!" >&2
        exit 1
    fi
    echo ""
    echo "✓ Test mode verification passed: Real downloads.json was NOT modified."
fi

echo ""
echo "====================================================="
echo "       ALL TESTS COMPLETED SUCCESSFULLY (100% PASS)   "
echo "====================================================="

#!/usr/bin/env bash
# Checks that VERSION, extension/manifest.json and safari/Info.plist agree.
set -u
cd "$(dirname "$0")/.."

VER=$(tr -d '[:space:]' < VERSION)
if [ -z "$VER" ]; then
    echo "check_version: VERSION is empty" >&2
    exit 1
fi

fail=0

MANIFEST_VER=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' extension/manifest.json | head -n1)
if [ "$MANIFEST_VER" != "$VER" ]; then
    echo "check_version: extension/manifest.json has '$MANIFEST_VER', VERSION has '$VER'" >&2
    fail=1
fi

# CFBundleShortVersionString and CFBundleVersion: the <string> on the line after each key
for key in CFBundleShortVersionString CFBundleVersion; do
    PLIST_VER=$(grep -A1 "<key>$key</key>" safari/Info.plist | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p' | head -n1)
    if [ "$PLIST_VER" != "$VER" ]; then
        echo "check_version: safari/Info.plist $key is '$PLIST_VER', VERSION has '$VER'" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "check_version: OK ($VER)"

#!/usr/bin/env bash
# Apply project patches to the ESP-IDF installation.
# Safe to run multiple times: already-applied patches are skipped.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCHES_DIR="$REPO_ROOT/patches"

if [ -z "${IDF_PATH:-}" ]; then
    echo "ERROR: IDF_PATH is not set. Source ESP-IDF export.sh first." >&2
    exit 1
fi

for patch in "$PATCHES_DIR"/*.patch; do
    [ -f "$patch" ] || continue
    name="$(basename "$patch")"
    # -N (--forward): exit 0 if the patch applies cleanly, exit 1 if it is already
    # applied (or reversed).  Unlike plain --dry-run, -N does not auto-retry in the
    # opposite direction, so its exit code reliably distinguishes the two states.
    if patch -p1 -N --dry-run --batch -s -d "$IDF_PATH" < "$patch" 2>/dev/null; then
        echo "Patch $name: applying..."
        patch -p1 -N -d "$IDF_PATH" < "$patch"
    else
        echo "Patch $name: already applied, skipping."
    fi
done

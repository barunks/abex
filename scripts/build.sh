#!/usr/bin/env bash
# =============================================================================
#  ABEX Build Script
#
#  Configures and builds the debug preset (default) or a named preset.
#  Safe to run from ANY directory — every path is absolute via $REPO.
#
#  Usage:
#    ./scripts/build.sh                  # debug (default)
#    ./scripts/build.sh release          # release
#    ./scripts/build.sh clang-asan       # Clang ASan + LSan + UBSan
#    ./scripts/build.sh clang-tsan       # Clang TSan
#    ./scripts/build.sh all              # all presets
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "$(realpath "$0")")/.." && pwd)"
PRESET="${1:-debug}"

GREEN='\033[0;32m'; RED='\033[0;31m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

ok()   { echo -e "  ${GREEN}✔  $*${RESET}"; }
fail() { echo -e "  ${RED}✘  $*${RESET}"; exit 1; }
say()  { echo -e "${DIM}  ℹ  $*${RESET}"; }

build_preset() {
    local p="$1"
    echo ""
    echo -e "${BOLD}── Preset: ${p} ──────────────────────────────────────────────────────${RESET}"
    say "Configuring ..."
    cmake --preset "$p" -S "$REPO" 2>&1 | tail -5
    say "Building ..."
    cmake --build --preset "$p" -- -j"$(nproc)"
    ok "Preset '${p}' built successfully."
}

cd "$REPO"

if [[ "$PRESET" == "all" ]]; then
    for p in debug release clang-asan clang-tsan; do
        build_preset "$p"
    done
else
    build_preset "$PRESET"
fi

echo ""
ok "Done. Binaries in $REPO/build/ (debug) or $REPO/build-<preset>/."

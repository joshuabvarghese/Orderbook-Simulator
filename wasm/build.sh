#!/usr/bin/env bash
# wasm/build.sh
#
# Run this ONCE on your Mac to compile the C++ engine to WebAssembly.
# Output: docs/orderbook.js + docs/orderbook.wasm
#
# Usage:
#   bash wasm/build.sh
#
# Requirements: nothing pre-installed — script handles everything.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMSDK_DIR="$ROOT/wasm/emsdk"
OUT_JS="$ROOT/docs/orderbook.js"
OUT_WASM="$ROOT/docs/orderbook.wasm"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}[build]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn] ${NC} $*"; }
die()   { echo -e "${RED}[error]${NC} $*"; exit 1; }

echo ""
echo "  ┌──────────────────────────────────────────────────────┐"
echo "  │   Order Book WASM Build                              │"
echo "  │   Compiles real C++ engine → WebAssembly             │"
echo "  └──────────────────────────────────────────────────────┘"
echo ""

# ── Step 1: Check Xcode tools ────────────────────────────────────────────────
if ! command -v git &>/dev/null; then
    die "git not found. Run: xcode-select --install"
fi
info "Xcode tools OK"

# ── Step 2: Install / update emsdk ──────────────────────────────────────────
if [ ! -d "$EMSDK_DIR" ]; then
    info "Cloning emsdk..."
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
else
    info "emsdk already cloned — pulling latest..."
    git -C "$EMSDK_DIR" pull --ff-only 2>/dev/null || true
fi

info "Installing Emscripten toolchain (this takes ~2 min first time)..."
"$EMSDK_DIR/emsdk" install  latest
"$EMSDK_DIR/emsdk" activate latest
source "$EMSDK_DIR/emsdk_env.sh" --build=Release > /dev/null 2>&1
info "Emscripten $(emcc --version | head -1)"

# ── Step 3: Compile ──────────────────────────────────────────────────────────
info "Compiling C++ → WASM..."

EXPORTED_FUNCTIONS='[
  "_ob_init","_ob_step",
  "_ob_bid_count","_ob_ask_count","_ob_trade_count_buf",
  "_ob_bid_price","_ob_bid_qty",
  "_ob_ask_price","_ob_ask_qty",
  "_ob_trade_price","_ob_trade_qty","_ob_trade_side",
  "_ob_best_bid","_ob_best_ask",
  "_ob_total_msgs","_ob_total_trades",
  "_ob_p50_ns","_ob_p90_ns","_ob_p99_ns","_ob_p999_ns",
  "_ob_mean_ns","_ob_pool_used","_ob_pool_cap"
]'

# Collapse to single line for shell
EXPORTED_FUNCTIONS=$(echo "$EXPORTED_FUNCTIONS" | tr -d '\n ')

emcc \
  "$ROOT/wasm/orderbook_wasm.cpp" \
  -I"$ROOT/include" \
  -std=c++17 \
  -O3 \
  -fno-exceptions \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME='"OrderBookModule"' \
  -s EXPORTED_FUNCTIONS="$EXPORTED_FUNCTIONS" \
  -s EXPORTED_RUNTIME_METHODS='["cwrap"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=268435456 \
  -s MAXIMUM_MEMORY=536870912 \
  -s ENVIRONMENT=web \
  -s NO_FILESYSTEM=1 \
  -s ASSERTIONS=0 \
  --closure 0 \
  -o "$OUT_JS"

# ── Step 4: Report ───────────────────────────────────────────────────────────
JS_SIZE=$(du -h "$OUT_JS"   | cut -f1)
WASM_SIZE=$(du -h "$OUT_WASM" | cut -f1)

echo ""
echo "  ┌──────────────────────────────────────────────────────┐"
echo "  │   Build complete ✓                                   │"
printf "  │   %-52s│\n" "docs/orderbook.js    $JS_SIZE"
printf "  │   %-52s│\n" "docs/orderbook.wasm  $WASM_SIZE"
echo "  ├──────────────────────────────────────────────────────┤"
echo "  │   Run the demo locally:                              │"
echo "  │                                                      │"
echo "  │     cd docs && python3 -m http.server 8080           │"
echo "  │     open http://localhost:8080                       │"
echo "  │                                                      │"
echo "  │   Or push to GitHub — Pages auto-deploys.            │"
echo "  └──────────────────────────────────────────────────────┘"
echo ""

#!/usr/bin/env bash
# scripts/check_regression.sh
#
# Runs the benchmark and fails if P99 exceeds the threshold.
# Used by CI to catch performance regressions.
#
# P99 threshold: 2000 ns  (2 µs)  — generous for shared CI runners

set -euo pipefail

THRESHOLD_NS=2000
MESSAGES=500000

echo "Running benchmark with ${MESSAGES} messages..."
OUTPUT=$(./orderbook "${MESSAGES}" 2>&1)
echo "${OUTPUT}"

# Extract P99 line from Benchmark 1 (first occurrence of "P99")
P99=$(echo "${OUTPUT}" | grep "P99 " | head -1 | grep -oP '\d+(?= ns)')

if [[ -z "${P99}" ]]; then
    echo "ERROR: Could not parse P99 from output"
    exit 1
fi

echo ""
echo "Parsed P99 = ${P99} ns  (threshold = ${THRESHOLD_NS} ns)"

if (( P99 > THRESHOLD_NS )); then
    echo "FAIL: P99 ${P99} ns exceeds threshold ${THRESHOLD_NS} ns"
    exit 1
else
    echo "PASS: P99 within threshold"
fi

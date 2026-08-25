#!/usr/bin/env bash
set -euo pipefail

preset="${1:-dev}"
cmake --preset "${preset}"
cmake --build --preset "${preset}" --parallel
ctest --test-dir "build/${preset}" --output-on-failure

#!/usr/bin/env bash
set -euo pipefail

generated="build/generated/python"
mkdir -p "${generated}"
protoc -I protocol --python_out "${generated}" protocol/poker.proto
PYTHONPATH="${generated}${PYTHONPATH:+:${PYTHONPATH}}" \
  python3 tools/load/active_table_load.py "$@"

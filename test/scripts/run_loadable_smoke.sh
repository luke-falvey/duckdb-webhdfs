#!/usr/bin/env bash
# Install the matching stock DuckDB into a virtualenv, start the clusters, and run loadable_smoke.py against them.
# HTTPFS=<path to httpfs.duckdb_extension> loads that file instead of installing httpfs from DuckDB's repository.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
VERSION="${DUCKDB_VERSION:-$(git -C duckdb describe --tags --exact-match)}"
VENV="$ROOT/build/loadable_smoke_venv"
python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet "duckdb==${VERSION#v}"
"$ROOT/test/scripts/webhdfs_up.sh"
trap '"$ROOT/test/scripts/webhdfs_down.sh"' EXIT
"$VENV/bin/python" -I test/scripts/loadable_smoke.py build/release/extension/webhdfs/webhdfs.duckdb_extension \
  --httpfs "${HTTPFS:-install}" --cluster webhdfs://localhost:9870

#!/usr/bin/env bash
# Start the clusters, run the integration tests, and tear the clusters down again.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
"$ROOT/test/scripts/webhdfs_up.sh"
trap '"$ROOT/test/scripts/webhdfs_down.sh"' EXIT
cd "$ROOT"
WEBHDFS_TEST_RUNNING=1 HADOOP_CONF_DIR="$ROOT/test/hadoop-conf" build/release/test/unittest "test/sql/integration/*"

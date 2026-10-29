#!/usr/bin/env bash
# Stop and remove the test clusters.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -f "$ROOT/build/webhdfs_standby.pid" ]; then
  kill "$(cat "$ROOT/build/webhdfs_standby.pid")" 2>/dev/null || true
  rm -f "$ROOT/build/webhdfs_standby.pid"
fi
docker compose -f "$ROOT/test/docker/docker-compose.yml" down -v

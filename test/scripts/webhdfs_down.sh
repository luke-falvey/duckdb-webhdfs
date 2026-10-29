#!/usr/bin/env bash
# Stop and remove the test clusters.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
docker compose -f "$ROOT/test/docker/docker-compose.yml" down -v

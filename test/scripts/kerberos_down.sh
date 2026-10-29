#!/usr/bin/env bash
# Stop the Kerberized HDFS and the test KDC.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
docker compose -f "$ROOT/test/docker-kerberos/docker-compose.yml" down
if [ -f "$ROOT/build/kerberos/krb5kdc.pid" ]; then
  kill "$(cat "$ROOT/build/kerberos/krb5kdc.pid")" 2>/dev/null || true
fi

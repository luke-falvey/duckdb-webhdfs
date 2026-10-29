#!/usr/bin/env bash
# Start the KDC and the Kerberized HDFS, run the Kerberos tests, and tear everything down again.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
"$ROOT/test/scripts/kerberos_up.sh"
trap '"$ROOT/test/scripts/kerberos_down.sh"' EXIT
cd "$ROOT"
export KRB5_CONFIG="$ROOT/build/kerberos/krb5.conf"
WEBHDFS_KERBEROS_TEST_RUNNING=1 HADOOP_CONF_DIR="$ROOT/test/hadoop-conf-kerberos" KRB5CCNAME="FILE:$ROOT/build/kerberos/alice.ccache" \
  build/release/test/unittest "test/sql/kerberos/webhdfs_kerberos.test"
WEBHDFS_KERBEROS_NO_TICKET=1 KRB5CCNAME="FILE:$ROOT/build/kerberos/missing.ccache" \
  build/release/test/unittest "test/sql/kerberos/webhdfs_kerberos_no_ticket.test"

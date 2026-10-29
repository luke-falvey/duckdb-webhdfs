#!/usr/bin/env bash
# Start the two test clusters (and the HttpFS gateway) and seed the fixtures the integration tests expect.
# Needs Docker and a built DuckDB shell (build/release/duckdb, used to generate the fixture files).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPOSE="docker compose -f $ROOT/test/docker/docker-compose.yml"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
FIXTURES="$ROOT/build/webhdfs_fixtures"

$COMPOSE up -d

wait_for() {
  local container=$1 url=$2
  for _ in $(seq 1 90); do
    # capture first: with pipefail, grep -q closing the pipe early would fail the check
    if curl -sf "$url" > /dev/null 2>&1 &&
      [[ "$(docker exec "$container" hdfs dfsadmin -safemode get 2>/dev/null)" == *OFF* ]]; then
      return 0
    fi
    sleep 2
  done
  echo "timed out waiting for $container" >&2
  docker logs "$container" | tail -50 >&2
  return 1
}
wait_for webhdfs-test "http://localhost:9870/webhdfs/v1/?op=GETFILESTATUS"
wait_for webhdfs-test-2 "http://localhost:19870/webhdfs/v1/?op=GETFILESTATUS"
# the DataNodes must have registered before files can be written
for container in webhdfs-test webhdfs-test-2; do
  for _ in $(seq 1 60); do
    if [[ "$(docker exec "$container" hdfs dfsadmin -report 2>/dev/null)" == *"Live datanodes (1)"* ]]; then break; fi
    sleep 2
  done
done
for _ in $(seq 1 60); do
  if curl -sf "http://localhost:14000/webhdfs/v1/?op=GETFILESTATUS&user.name=hadoop" > /dev/null; then break; fi
  sleep 2
done

# fixture files, generated locally
rm -rf "$FIXTURES" && mkdir -p "$FIXTURES/hive"
"$DUCKDB" -no-agent -c "
COPY (SELECT range AS id, 'name_' || range AS name FROM range(5)) TO '$FIXTURES/people.csv';
COPY (SELECT range AS id, range * 2 AS doubled, 'value_' || range AS label FROM range(100000))
  TO '$FIXTURES/numbers.parquet';
COPY (SELECT range AS id, 'nested' AS source FROM range(3)) TO '$FIXTURES/nested.csv';
COPY (SELECT range AS id, 'deeper' AS source FROM range(2)) TO '$FIXTURES/deeper.csv';
COPY (SELECT 1 AS a, 'x' AS b UNION ALL SELECT 2, 'y') TO '$FIXTURES/records.json';
COPY (SELECT 'hidden' AS what) TO '$FIXTURES/hidden.csv';
COPY (SELECT 'unicode' AS what, 42 AS n) TO '$FIXTURES/unicode.csv';
COPY (SELECT 'secret' AS what) TO '$FIXTURES/secret.csv';
COPY (SELECT 'other cluster' AS source, range AS id FROM range(4)) TO '$FIXTURES/other.csv';
COPY (
  SELECT year::VARCHAR AS year, month::VARCHAR AS month, (year * 100 + month) * 10 + i AS id
  FROM range(2022, 2025) t1(year), range(1, 13) t2(month), range(10) t3(i)
) TO '$FIXTURES/hive' (FORMAT parquet, PARTITION_BY (year, month));
" > /dev/null

docker cp "$FIXTURES" webhdfs-test:/tmp/fixtures > /dev/null
docker cp "$FIXTURES/other.csv" webhdfs-test-2:/tmp/other.csv > /dev/null

docker exec webhdfs-test bash -e -c '
F=/tmp/fixtures
# a snapshottable directory that holds snapshots cannot be deleted
hdfs dfs -deleteSnapshot /test/snap s1 > /dev/null 2>&1 || true
hdfs dfsadmin -disallowSnapshot /test/snap > /dev/null 2>&1 || true
hdfs dfs -rm -r -f -skipTrash /test /user/alice > /dev/null 2>&1 || true
hdfs dfs -mkdir -p /test/data/sub/deep "/test/data/space dir" /test/hive /test/private /test/quota /test/snap /test/out /user/alice /user/hadoop
hdfs dfs -put "$F/people.csv" "$F/numbers.parquet" "$F/records.json" /test/data/
hdfs dfs -put "$F/nested.csv" /test/data/sub/
hdfs dfs -put "$F/deeper.csv" /test/data/sub/deep/
hdfs dfs -put "$F/hidden.csv" /test/data/.hidden.csv
hdfs dfs -put "$F/hidden.csv" /test/data/_temporary.csv
hdfs dfs -touchz /test/data/_SUCCESS
hdfs dfs -put "$F/unicode.csv" "/test/data/space dir/ünï cødé.csv"
hdfs dfs -put "$F/hive/"* /test/hive/
hdfs dfs -put "$F/secret.csv" /test/private/
hdfs dfs -chmod 700 /test/private
hdfs dfs -chmod 777 /test/out
hdfs dfs -chown alice /user/alice
hdfs dfs -put "$F/people.csv" /test/quota/
hdfs dfsadmin -setQuota 100 /test/quota
hdfs dfsadmin -setSpaceQuota 1g /test/quota
hdfs dfs -put "$F/people.csv" /test/snap/
hdfs dfsadmin -allowSnapshot /test/snap > /dev/null
hdfs dfs -createSnapshot /test/snap s1 > /dev/null
hdfs dfs -setfattr -n user.color -v blue /test/data/people.csv
hdfs dfs -setfacl -m user:alice:r-- /test/data/people.csv
# 200 small files: many directory listing pages (dfs.ls.limit=5)
mkdir -p /tmp/many && for i in $(seq -w 1 200); do echo "n" > /tmp/many/f$i.txt; echo "$i" >> /tmp/many/f$i.txt; done
hdfs dfs -mkdir -p /test/many && hdfs dfs -put /tmp/many/* /test/many/
'
docker exec webhdfs-test-2 bash -e -c '
hdfs dfs -rm -r -f -skipTrash /other > /dev/null 2>&1 || true
hdfs dfs -mkdir -p /other
hdfs dfs -put /tmp/other.csv /other/data.csv
'
# a fake standby NameNode for the failover tests
STANDBY_PID_FILE="$ROOT/build/webhdfs_standby.pid"
if [ -f "$STANDBY_PID_FILE" ]; then kill "$(cat "$STANDBY_PID_FILE")" 2>/dev/null || true; fi
nohup python3 -I "$ROOT/test/scripts/standby_namenode.py" 19999 > /dev/null 2>&1 &
echo $! > "$STANDBY_PID_FILE"

echo "WebHDFS test clusters are up: webhdfs://localhost:9870 (HttpFS on 14000) and webhdfs://localhost:19870"

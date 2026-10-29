#!/usr/bin/env bash
# WebHDFS benchmark: wall time and NameNode/DataNode request counts for listing-heavy and read-heavy queries.
# Runs against the primary test cluster (test/scripts/webhdfs_up.sh). Each query runs in a fresh DuckDB process,
# so the request counts (from webhdfs_namenodes()) are per query.
#
#   test/bench/webhdfs_bench.sh [--seed]
#
# --seed (re)writes the benchmark table: a Hive-partitioned Parquet table with 1,680 partition directories
# (5 years x 12 months x 28 days), written through webhdfs itself.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
NN="webhdfs://localhost:9870"
TABLE="$NN/bench/events"

if [ "${1:-}" = "--seed" ]; then
  docker exec webhdfs-test bash -c 'hdfs dfs -rm -r -f -skipTrash /bench > /dev/null 2>&1; hdfs dfs -mkdir -p /bench && hdfs dfs -chmod 777 /bench'
  "$DUCKDB" -no-agent -c "
    SET webhdfs_user_name = 'hadoop';
    COPY (
      SELECT y AS year, m AS month, d AS day, (y * 10000 + m * 100 + d) * 100 + i AS id, random() AS value
      FROM range(2020, 2025) t1(y), range(1, 13) t2(m), range(1, 29) t3(d), range(100) t4(i)
    ) TO '$TABLE' (FORMAT parquet, PARTITION_BY (year, month, day));" > /dev/null
fi

printf "%-62s %9s %9s %9s\n" "query" "seconds" "NN reqs" "DN reqs"
run() {
  local label=$1 settings=$2 query=$3
  local start end out
  start=$(date +%s.%N)
  out=$("$DUCKDB" -no-agent -csv -noheader -c "
    SET webhdfs_user_name = 'hadoop';
    $settings
    CREATE TEMP TABLE result AS $query;
    SELECT coalesce(sum(requests), 0), coalesce(max(datanode_requests), 0) FROM webhdfs_namenodes();")
  end=$(date +%s.%N)
  printf "%-62s %9.2f %9s %9s\n" "$label" "$(echo "$end - $start" | bc)" "$(echo "$out" | cut -d, -f1)" "$(echo "$out" | cut -d, -f2)"
}

Q_ALL="SELECT count(*), sum(id) FROM read_parquet('$TABLE/*/*/*/*.parquet', hive_partitioning = true)"
Q_MONTH_WILDCARD="SELECT count(*), sum(id) FROM read_parquet('$TABLE/*/*/*/*.parquet', hive_partitioning = true) WHERE year = 2023 AND month = 5"
Q_MONTH_NAMED="SELECT count(*), sum(id) FROM read_parquet('$TABLE/year=2023/month=5/*/*.parquet', hive_partitioning = true)"
Q_DAYS_BRACES="SELECT count(*), sum(id) FROM read_parquet('$TABLE/year=2023/month={5,6}/day={1,15}/*.parquet', hive_partitioning = true)"

run "full scan, 1680 files, list parallelism 1" "SET webhdfs_list_parallelism = 1;" "$Q_ALL"
run "full scan, 1680 files, list parallelism 16 (default)" "" "$Q_ALL"
run "one month, wildcards + WHERE" "" "$Q_MONTH_WILDCARD"
run "one month, named in the path" "" "$Q_MONTH_NAMED"
run "4 days, named with braces" "" "$Q_DAYS_BRACES"
run "glob() of the whole table" "" "SELECT count(*) FROM glob('$TABLE/**/*.parquet')"
run "webhdfs_ls of the whole table" "" "SELECT count(*) FROM webhdfs_ls('$TABLE/**')"

BIG="$NN/test/data/numbers.parquet"
run "numbers.parquet, all columns" "" "SELECT sum(id), max(label) FROM '$BIG'"
run "numbers.parquet, no redirect reuse" "SET webhdfs_reuse_redirects = false;" "SELECT sum(id), max(label) FROM '$BIG'"
run "people.csv, read-ahead off" "SET webhdfs_read_buffer_size = 0;" "SELECT count(*) FROM read_csv('$NN/test/data/people.csv')"

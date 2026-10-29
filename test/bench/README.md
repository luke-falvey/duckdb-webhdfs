# WebHDFS benchmark

`webhdfs_bench.sh` runs listing-heavy and read-heavy queries against the primary test cluster and reports, per
query, the wall time and the number of NameNode and DataNode requests (from `webhdfs_namenodes()`; each query runs in
a fresh DuckDB process).

```sh
test/scripts/webhdfs_up.sh            # start and seed the test clusters
test/bench/webhdfs_bench.sh --seed    # --seed writes the 1,680-partition benchmark table first
```

The table is Hive-partitioned Parquet: 5 years x 12 months x 28 days = 1,680 partition directories with one file
each. The test cluster lists at most 5 entries per `LISTSTATUS_BATCH` page (`dfs.ls.limit`), so listings take more
requests than on a real cluster (default 1,000 per page).

## Results

Single-node Docker cluster on the same machine (4 cores), so round trips cost well under a millisecond. On a real
network every request costs 0.5-5 ms, which makes the request counts the number to watch.

| query | seconds | NameNode requests | DataNode requests |
|---|---:|---:|---:|
| full scan, 1,680 files, list parallelism 1 | 6.82 | 3,736 | 3,360 |
| full scan, 1,680 files, list parallelism 16 (default) | 4.93 | 3,736 | 3,360 |
| one month, wildcards + `WHERE year = 2023 AND month = 5` | 1.23 | 2,085 | 57 |
| one month, named in the path (`year=2023/month=5/*/*.parquet`) | 0.15 | 62 | 56 |
| 4 days, named with braces (`month={5,6}/day={1,15}`) | 0.09 | 8 | 8 |
| `glob()` of the whole table | 1.31 | 2,056 | 0 |
| `webhdfs_ls` of the whole table | 1.23 | 2,056 | 0 |
| `numbers.parquet` (1.3 MB), all columns | 0.09 | 2 | 3 |
| the same, `webhdfs_reuse_redirects = false` | 0.11 | 4 | 3 |
| `people.csv`, `webhdfs_read_buffer_size = 0` | 0.10 | 3 | 1 |

What the numbers show:

- **Partition filters in `WHERE` don't reduce listing.** DuckDB (v2.0.0) lists every file of a Hive-partitioned
  read while binding the query, before filters are applied, so `WHERE year = 2023 AND month = 5` still lists all
  1,746 directories; it only skips reading the other files. Naming the partitions in the path lists only those
  directories: 2,085 requests become 62, and braces for several values go further (8).
- **Each file costs one NameNode request** (the `OPEN` that redirects to a DataNode). The redirect is remembered per
  file version and user, so a reader opening a file twice, or reading several ranges, goes straight to the DataNode
  afterwards. Without that, the second read of `numbers.parquet` costs another NameNode round trip.
- **Listing in parallel** matters more with real network latency than here.

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
| full scan, 1,680 files, list parallelism 1 | 5.74 | 3,736 | 1,680 |
| full scan, 1,680 files, list parallelism 16 (default) | 3.45 | 3,736 | 1,680 |
| one month, wildcards + `WHERE year = 2023 AND month = 5` | 1.30 | 2,085 | 29 |
| one month, named in the path (`year=2023/month=5/*/*.parquet`) | 0.13 | 62 | 28 |
| 4 days, named with braces (`month={5,6}/day={1,15}`) | 0.08 | 8 | 4 |
| `glob()` of the whole table | 1.04 | 2,056 | 0 |
| `webhdfs_ls` of the whole table | 1.05 | 2,056 | 0 |
| `numbers.parquet` (1.3 MB), all columns | 0.10 | 2 | 3 |
| the same, `webhdfs_reuse_redirects = false` | 0.09 | 4 | 3 |
| `people.csv`, `webhdfs_read_buffer_size = 0` | 0.07 | 3 | 1 |

What the numbers show:

- **Partition filters in `WHERE` don't reduce listing.** DuckDB (v2.0.0) lists every file of a Hive-partitioned
  read while binding the query, before filters are applied, so `WHERE year = 2023 AND month = 5` still lists all
  1,746 directories; it only skips reading the other files. Naming the partitions in the path lists only those
  directories: 2,085 requests become 62, and braces for several values go further (8).
- **Each file costs one NameNode request** (the `OPEN` that redirects to a DataNode). The redirect is remembered per
  file version and user, so a reader opening a file twice, or reading several ranges, goes straight to the DataNode
  afterwards. Without that, the second read of `numbers.parquet` costs another NameNode round trip.
- **A small file costs one DataNode request.** DuckDB's Parquet reader opens each file twice (once to read its
  metadata while binding the query, once to scan it). Files up to the read-ahead size (1 MiB) are fetched whole and
  kept in a small per-cluster cache (64 MiB), so the scan's open costs nothing; for larger files, a read near the end
  fetches the file's tail, which holds the Parquet footer and metadata. This halved the DataNode requests of the full
  scan (3,360 to 1,680) and cut its time by about 30% (4.9 s to 3.5 s).
- **Listing in parallel** matters more with real network latency than here.

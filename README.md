# DuckDB WebHDFS extension

`webhdfs` lets DuckDB read and write HDFS over its REST API,
[WebHDFS](https://hadoop.apache.org/docs/stable/hadoop-project-dist/hadoop-hdfs/WebHDFS.html), using
`webhdfs://` and `swebhdfs://` URLs. Every HTTP request goes through DuckDB's
[httpfs](https://duckdb.org/docs/stable/core_extensions/httpfs/overview) extension, so httpfs's settings, proxies,
TLS options and `http` secrets apply. There are no native Hadoop libraries and no JVM.

```sql
LOAD webhdfs;   -- also loads httpfs (and installs it) if autoloading is allowed

SELECT * FROM 'webhdfs://namenode:9870/data/events/*.parquet';
SELECT * FROM read_csv('swebhdfs://namenode:9871/data/users.csv');
COPY (SELECT * FROM big_table) TO 'webhdfs://namenode:9870/out/table.parquet';

SELECT path, size, owner FROM webhdfs_ls('webhdfs://namenode:9870/data/**') ORDER BY size DESC LIMIT 10;
```

## What works

- Reading every format DuckDB reads (Parquet, CSV, JSON, ...): parallel range reads, read-ahead for small sequential
  reads, and reuse of DataNode redirects so repeated reads of a file skip the NameNode.
- Writing with `COPY ... TO`, including `PARTITION_BY`, `OVERWRITE`, `OVERWRITE_OR_IGNORE` and `USE_TMP_FILE`. HDFS
  files are append-only: a file is created with its first 64 MiB, then appended to.
- Globbing: `*`, `?`, `[abc]`, `**` (any depth), `{a,b}` alternation. Hidden entries (`_SUCCESS`, `_temporary`,
  `.crc` files) are skipped unless named. Listings are paged (`LISTSTATUS_BATCH`) and run in parallel, and files come
  back in a stable order.
- Several clusters in one query, each with its own credentials; HA NameNode lists with failover, from a secret or from
  `hdfs-site.xml`.
- Authentication: Hadoop simple auth (`user.name`), proxy users (`doas`), delegation tokens, Kerberos (SPNEGO, using
  your ticket cache), and gateways (HttpFS, Apache Knox) through `http` secrets.
- Metadata functions for listings, file status, content summaries, quotas, block locations, checksums, ACLs,
  extended attributes, storage policies, snapshots and server defaults.

## URLs

`webhdfs://host[:port]/path` talks HTTP to the NameNode (default port 9870), and `swebhdfs://host[:port]/path` talks
HTTPS (default port 9871). These are Hadoop's own schemes, so paths copied from `hadoop fs` work unchanged. Paths may
contain spaces and any UTF-8 characters.

If `host` has no port and names an HA nameservice in your `hdfs-site.xml` (`HADOOP_CONF_DIR`, else
`HADOOP_HOME/etc/hadoop`), its NameNodes are used: `webhdfs://mycluster/data/file.parquet`.

## Connection settings: the `webhdfs` secret

```sql
CREATE SECRET prod_hdfs (
    TYPE webhdfs,
    SCOPE 'webhdfs://mycluster',
    USER_NAME 'analytics',
    NAMENODES ['nn1.example.com:9870', 'nn2.example.com:9870']
);
```

A secret applies to the URLs its `SCOPE` prefixes (the longest match wins; the default scope is every `webhdfs://`
and `swebhdfs://` URL). Options:

| Option | Meaning |
|---|---|
| `USER_NAME` | User for simple authentication (`user.name=`). |
| `PROXY_USER` | Act as this user (`doas=`); the authenticated user must be allowed to impersonate. |
| `DELEGATION_TOKEN` | A delegation token (`delegation=`); implies `AUTH 'token'`. Redacted in `duckdb_secrets()`. |
| `AUTH` | `simple`, `token`, `kerberos` or `none`. |
| `NAMENODES` | NameNode HTTP addresses (`host:port`) of an HA cluster, tried in order. |
| `GATEWAY_PATH` | Path prefix before `/webhdfs/v1`, e.g. `/gateway/default` for Apache Knox. |
| `DATANODE_ADDRESS_MAP` | `MAP {'dn1.internal:9864': 'localhost:19864'}` rewrites DataNode redirects (Docker, Kubernetes, NAT). A key without a port matches any port. |
| `USE_SSL` | Override the scheme's choice of HTTP/HTTPS. |
| `KERBEROS_SERVICE` | Service name of the NameNode's HTTP principal (default `HTTP`). |
| `PERMISSION`, `REPLICATION`, `BLOCK_SIZE` | Options for files created by writes. |

Without a secret, the user is `webhdfs_user_name`, else `HADOOP_USER_NAME`, else the OS user (`HADOOP_PROXY_USER`
sets the proxy user). Kerberos is selected when `hadoop.security.authentication` is `kerberos` in `core-site.xml`.

TLS (CA bundle, certificate checks), HTTP proxies, timeouts and retries are httpfs settings (`http_timeout`,
`http_retries`, `http_proxy`, `ca_cert_file`, ...). Extra headers and bearer tokens, e.g. for a Knox gateway, go in
an httpfs `http` secret scoped to the `webhdfs://` URL:

```sql
CREATE SECRET knox (TYPE http, SCOPE 'swebhdfs://knox.example.com:8443',
                    EXTRA_HTTP_HEADERS MAP {'Authorization': 'Basic YWxpY2U6c2VjcmV0'});
CREATE SECRET knox_path (TYPE webhdfs, SCOPE 'swebhdfs://knox.example.com:8443', AUTH 'none',
                         GATEWAY_PATH '/gateway/default');
```

### Kerberos

With `AUTH 'kerberos'`, the extension authenticates with SPNEGO using your Kerberos ticket cache: run `kinit` first;
`KRB5CCNAME` picks the cache and `KRB5_CONFIG` the `krb5.conf`. It fetches one delegation token per cluster with
SPNEGO and uses it for later requests and DataNode redirects, fetching a new one after 12 hours or when the server
rejects it. GSSAPI is loaded from the system (`libgssapi_krb5`, or `GSS.framework` on macOS) only when Kerberos is
used. Kerberos is not supported on Windows yet.

### Several clusters and HA

Every URL names its cluster, so a query can mix clusters, each using the secret that matches its URL:

```sql
CREATE SECRET a (TYPE webhdfs, SCOPE 'webhdfs://prod-nn:9870', USER_NAME 'etl');
CREATE SECRET b (TYPE webhdfs, SCOPE 'webhdfs://dr-cluster', NAMENODES ['dr1:9870', 'dr2:9870'], AUTH 'kerberos');
SELECT * FROM 'webhdfs://prod-nn:9870/t/*.parquet' p JOIN 'webhdfs://dr-cluster/t/*.parquet' d USING (id);
```

For a NameNode list, the NameNode that last answered as active is tried first. Standby and observer NameNodes, and
NameNodes that can't be reached, make the request move to the next one; `RetriableException` and
`SafeModeException` are retried with backoff. `webhdfs_namenodes()` shows what each cluster resolved to:

```sql
SELECT endpoint, namenode, state, active, requests, last_error FROM webhdfs_namenodes();
```

## Metadata functions

Each table function takes a URL. It can also take a column, as in
`SELECT ... FROM webhdfs_ls('webhdfs://nn:9870/d') l, webhdfs_stat(l.path) s`.

| Function | Returns |
|---|---|
| `webhdfs_ls(url, include_hidden :=, skip_permission_errors :=)` | A directory's children, a glob's matches (directories included), or a file itself. Columns: `path`, `name`, `type`, `size`, `owner`, `group`, `permissions` (`rw-r--r--`), `mode`, `replication`, `block_size`, `last_modified`, `last_accessed`, `file_id`, `children_num`, `storage_policy`, `ec_policy`. |
| `webhdfs_stat(url)` | The same columns for one path. |
| `webhdfs_exists(url)` | `BOOLEAN` (scalar). |
| `webhdfs_content_summary(url)` | Recursive totals: `length`, `file_count`, `directory_count`, `space_consumed`, `quota`, `space_quota`, `erasure_coding_policy`. |
| `webhdfs_quota_usage(url)` | `file_and_directory_count`, `quota`, `space_consumed`, `space_quota`, `type_quotas` (per storage type). |
| `webhdfs_block_locations(url, offset :=, length :=)` | One row per block: `offset`, `length`, `hosts`, `names`, `topology_paths`, `storage_types`, `corrupt`. |
| `webhdfs_checksum(url)` | `algorithm`, `bytes`, `length`. Equal for files with the same content and block size. |
| `webhdfs_acl(url)` | `owner`, `group`, `permissions`, `sticky_bit`, `entries`. |
| `webhdfs_xattrs(url, names :=)` | Extended attributes: `name`, `value` (BLOB). |
| `webhdfs_storage_policy(url)` | `id`, `name`, `storage_types`, fallbacks, `copy_on_create_file`. |
| `webhdfs_snapshots(url)` | Snapshots of a directory: `path`, `name`, `snapshot_id`, `created`. |
| `webhdfs_snapshottable_directories(url)` | Snapshottable directories of the cluster. |
| `webhdfs_server_defaults(url)` | Cluster defaults: block size, replication, checksum type, trash interval, ... |
| `webhdfs_home_directory(url)`, `webhdfs_trash_root(url)` | URLs (scalars). |
| `webhdfs_namenodes()` | Per cluster and NameNode: state, request counts, last error, authentication, where the NameNode list came from. |

Missing paths raise an error (except in `webhdfs_exists` and in globs, which match nothing). Permission errors fail
the query; `skip_permission_errors := true` (or `SET webhdfs_skip_permission_errors = true`) skips unreadable
directories below the listed path instead.

## Partitioned (Hive) data

```sql
SELECT * FROM read_parquet('webhdfs://nn:9870/warehouse/events/*/*/*.parquet', hive_partitioning = true)
WHERE year = 2024 AND month = 10;
```

This works and reads only the matching files, but DuckDB lists every partition directory while binding the query,
before it looks at the filters (this is DuckDB's multi-file reader, in v2.0.0, for any filesystem). On a big table
that is one NameNode request per directory. When you know the partitions, name them in the path: only those
directories are listed, and directories named with braces are visited without listing their parent.

```sql
SELECT * FROM read_parquet('webhdfs://nn:9870/warehouse/events/year=2024/month={10,11}/*.parquet',
                           hive_partitioning = true);
```

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `webhdfs_list_parallelism` | 16 | Concurrent directory listings in globs and recursive listings. |
| `webhdfs_include_hidden` | false | Include `_`/`.` entries in globs and listings. |
| `webhdfs_skip_permission_errors` | false | Skip unreadable directories in globs and listings. |
| `webhdfs_read_buffer_size` | 1 MiB | Minimum read size; small sequential reads fetch this much ahead. 0 turns read-ahead off. |
| `webhdfs_write_chunk_size` | 64 MiB | Data buffered per file being written before it is sent. |
| `webhdfs_reuse_redirects` | true | Reuse a file's DataNode redirect for later reads of it. |
| `webhdfs_user_name` | (unset) | User for simple authentication when no secret sets one. |

## Performance notes

- Each file read costs one NameNode request (`OPEN`, answered by a redirect to a DataNode); later reads of the same
  file go straight to the DataNode. Files found by a glob or listing are opened without asking for their status
  again.
- Files up to `webhdfs_read_buffer_size` are fetched in one request and kept in a small cache (64 MiB per cluster,
  keyed by user, path and modification time), so a reader that opens a file twice (DuckDB's Parquet reader does)
  reads it once. A read near the end of a larger file fetches its tail, where Parquet keeps its footer.
- Listings are paged and run in parallel (`webhdfs_list_parallelism`); request counts matter more than bandwidth for
  many small files. See [test/bench](test/bench/README.md) for numbers.
- DuckDB's external file cache works with WebHDFS files (they carry a modification-time version tag).

## Limitations

- Files can't be written at arbitrary offsets or truncated (HDFS is append-only). A chunk that fails to `APPEND` is
  not retried (a retry could duplicate data); the write fails with an error.
- Partition filters don't reduce listing (see above).
- Writes buffer up to `webhdfs_write_chunk_size` per open file; `PARTITION_BY` with many open partitions multiplies
  that.
- Not tested yet: `swebhdfs://` against a TLS cluster, a real Apache Knox (gateway paths and `http` secret headers
  were only checked against a stand-in server), Kerberos with a gateway that authenticates every request instead of
  issuing tokens, and the loadable extension on macOS and Windows (on Linux it is tested in a stock DuckDB 2.0.0).
- No metadata-changing functions (`SETPERMISSION`, `SETOWNER`, quotas, snapshots).

## Building

Targets DuckDB v2.0.0 (the `duckdb` submodule). httpfs is built alongside, from the commit DuckDB v2.0.0 pins, for the
test runner and the bundled shell only: the extension itself doesn't link it.

```sh
git submodule update --init --recursive
GEN=ninja make            # needs CMake, a C++17 compiler, and libcurl + OpenSSL headers (for httpfs)
```

The CI distribution build gets curl and OpenSSL from vcpkg (`vcpkg.json`). Artifacts:

- `build/release/duckdb`: shell with webhdfs and httpfs linked in
- `build/release/extension/webhdfs/webhdfs.duckdb_extension`: the loadable extension (about 13 MB, almost all of it
  the parts of DuckDB core it links statically, like every C++ extension; Linux release builds strip its symbol
  table)
- `build/release/test/unittest`: the test runner

## Testing

```sh
make test_unit                          # URL, glob, Hadoop config and JSON tests (no DuckDB build needed)
build/release/test/unittest "test/sql/*"  # SQL tests; cluster tests are skipped without a cluster
test/scripts/run_webhdfs_tests.sh       # Docker clusters + integration tests (needs Docker)
test/scripts/run_kerberos_tests.sh      # KDC + Kerberized cluster + Kerberos tests (needs Docker and MIT Kerberos)
```

See [test/README.md](test/README.md) for what the clusters contain and how to run tests by hand.

## Layout

- `src/webhdfs_extension.cpp`: registration (filesystem, settings, secret type, functions, httpfs loading).
- `src/webhdfs_filesystem.cpp`: the DuckDB `FileSystem`; `webhdfs_file_list.cpp`: lazy glob results.
- `src/webhdfs_client.cpp`: REST client (NameNode failover, redirects, Kerberos tokens, errors);
  `webhdfs_http.cpp`: requests through httpfs.
- `src/webhdfs_walk.cpp`: parallel glob walk; `webhdfs_glob.cpp`: pattern matching.
- `src/webhdfs_config.cpp`: settings and per-cluster configuration; `webhdfs_secret.cpp`: the secret type;
  `webhdfs_hadoop_config.cpp`: `hdfs-site.xml` reading.
- `src/webhdfs_functions.cpp`: metadata SQL functions.
- `src/webhdfs_url.cpp`, `webhdfs_json.cpp`, `webhdfs_kerberos.cpp`: URLs, JSON parsing, SPNEGO tokens.
- `docs/PLAN.md`: the design plan and how the implementation differs from it.

# DuckDB WebHDFS extension

`webhdfs` lets DuckDB read and write HDFS over its REST API,
[WebHDFS](https://hadoop.apache.org/docs/stable/hadoop-project-dist/hadoop-hdfs/WebHDFS.html), using
`webhdfs://` and `swebhdfs://` URLs. Every HTTP request goes through DuckDB's
[httpfs](https://duckdb.org/docs/stable/core_extensions/httpfs/overview) extension, so httpfs's settings, proxies,
TLS options and `http` secrets apply. There are no native Hadoop libraries and no JVM.

```sql
LOAD webhdfs;   -- also loads httpfs if autoloading is allowed (installed on first use if needed)

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
  `.crc` files) are skipped unless named. Listings are paged (`LISTSTATUS_BATCH`) and run in parallel, and the files a
  glob expands to (for `read_*` and `glob()`) come back sorted by full path (byte order), however many listings run at
  once. `webhdfs_ls`, which also returns directories, has no defined row order: use `ORDER BY`.
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

A secret applies to the URLs its `SCOPE` covers; the longest matching scope wins, and the default scope is every
`webhdfs://` and `swebhdfs://` URL. Unlike plain prefix scopes, a scope's scheme and authority must match the URL's
whole scheme and authority (case-insensitively): `webhdfs://prod` covers `webhdfs://prod/...` but not
`webhdfs://prod2/...` or `webhdfs://prod:9870/...`. A path after the authority, as in `webhdfs://prod/logs`, is a
plain prefix of the URL's path.

The authority doesn't have to be a host: a secret with `NAMENODES` turns any name into an alias for a cluster, so
`webhdfs://prod/data/x.parquet` goes to the NameNodes of the secret scoped to `webhdfs://prod`.

Options:

| Option | Meaning |
|---|---|
| `USER_NAME` | User for simple authentication (`user.name=`). |
| `PROXY_USER` | Act as this user (`doas=`); the authenticated user must be allowed to impersonate. |
| `DELEGATION_TOKEN` | A delegation token (`delegation=`); implies `AUTH 'token'`. Redacted in `duckdb_secrets()`. Not allowed with another `AUTH`. |
| `AUTH` | `simple`, `token` (requires `DELEGATION_TOKEN`), `kerberos` or `none`. |
| `NAMENODES` | NameNode HTTP addresses (`host:port`, no scheme or path) of an HA cluster, tried in order. |
| `GATEWAY_PATH` | Path prefix before `/webhdfs/v1`, e.g. `/gateway/default` for Apache Knox. |
| `DATANODE_ADDRESS_MAP` | `MAP {'dn1.internal:9864': 'localhost:19864'}` rewrites DataNode redirects (Docker, Kubernetes, NAT). A key without a port matches any port. |
| `USE_SSL` | Override the scheme's choice of HTTP/HTTPS. |
| `KERBEROS_SERVICE` | Service name of the NameNode's HTTP principal (default `HTTP`). |
| `PERMISSION`, `REPLICATION`, `BLOCK_SIZE` | Options for files created by writes (octal string, BIGINT, BIGINT). |

`CREATE SECRET` checks the `AUTH` value and its combination with `DELEGATION_TOKEN`, that each `NAMENODES` entry is
`host:port`, that `REPLICATION` (1 to 32767) and `BLOCK_SIZE` are positive, and that each `SCOPE` starts with
`webhdfs://` or `swebhdfs://` (the whole scheme: `'webhdfs:/'` or `'WEBHDFS'` is an error), so those mistakes fail
when the secret is created, not on first use. `PERMISSION` and
`GATEWAY_PATH` are passed to the server as given.

State kept per cluster (Kerberos delegation tokens, reused DataNode redirects, the small-file cache) is kept per
identity: auth mode, user, proxy user, Kerberos principal and `http` secret. Two secrets for the same cluster with
different `PROXY_USER`s never share a token or cached data.

Persistent `webhdfs` secrets are stored as ordinary key-value secrets, so sessions that don't load webhdfs can still
read their other persistent secrets. After a restart `which_secret()` reports core's prefix matching for them; the
extension itself always matches scopes by whole authority. Persistent secrets created by earlier development builds
used a custom format that only sessions with webhdfs loaded can read: re-create them (`CREATE OR REPLACE PERSISTENT
SECRET`) to store them in the new format.

When the matching secret doesn't set `USER_NAME` (or there is no secret), the user is `webhdfs_user_name`, else
`HADOOP_USER_NAME`, else the OS user; when it doesn't set `PROXY_USER`, `HADOOP_PROXY_USER` sets the proxy user. The auth mode is the secret's `AUTH`, else `token` if it has a `DELEGATION_TOKEN`, else
`kerberos` if `hadoop.security.authentication` is `kerberos` in `core-site.xml` (with or without a secret), else
`simple`.

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
`KRB5CCNAME` picks the cache and `KRB5_CONFIG` the `krb5.conf`. It fetches one delegation token per cluster and identity
(principal, proxy user) with SPNEGO and uses it for later requests and DataNode redirects, fetching a new one after 12 hours or when the server
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
`SELECT ... FROM webhdfs_ls('webhdfs://nn:9870/d') l, webhdfs_stat(l.path) s`. Besides the columns listed below,
every table function but `webhdfs_namenodes()` starts with the URL it describes: `path`, or `endpoint` for
`webhdfs_server_defaults`.

| Function | Returns |
|---|---|
| `webhdfs_ls(url, include_hidden :=, skip_permission_errors :=)` | A directory's children, a glob's matches (directories included), or a file itself. Columns: `path`, `name`, `type`, `size`, `owner`, `group`, `permissions` (`rw-r--r--`), `mode`, `replication`, `block_size`, `last_modified`, `last_accessed`, `file_id`, `children_num`, `storage_policy`, `ec_policy`. `last_modified` and `last_accessed` are `TIMESTAMP WITH TIME ZONE`; `last_accessed` is NULL for directories and when the NameNode doesn't track access times. |
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
| `webhdfs_namenodes()` | One row per cluster and NameNode in use by this database: `endpoint`, `namenode`, `state`, `active`, `requests`, `last_error`, `datanode_requests`, `auth_mode`, `namenode_source`, `secret_name`, `batched_listing`. `auth_mode`, `namenode_source` and `secret_name` are what this session would use now for the cluster root (`/`), not for a path below it: a secret scoped to `webhdfs://nn/data` is not shown, even for the NameNode its requests went to. They are NULL on a NameNode the root's configuration doesn't name (one from a dropped secret, another session's secret or a path-scoped secret's `NAMENODES`), when the access settings refuse the cluster root, and when the secret for it is invalid. |

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
| `webhdfs_small_file_cache_size` | 64 MiB | Memory per cluster for whole small files (see Performance notes). 0 turns the cache off. Database-wide: `SET` changes it for every session (`SET SESSION` is an error) and applies at once. |
| `webhdfs_user_name` | (unset) | User for simple authentication when no secret sets one. |

## Autocomplete

In the DuckDB shell (or anything using DuckDB's `autocomplete` extension, e.g. `sql_auto_complete`), Tab completes
`webhdfs://` and `swebhdfs://` URLs inside quotes:

- After `webhdfs://`: the cluster names this session knows, from `webhdfs` secret scopes (aliases), the HA
  nameservices in `hdfs-site.xml`, and clusters already used. `'webhdfs://pr` + Tab gives `'webhdfs://prod/`.
- After a cluster: the entries of the directory typed so far. Directories end in `/`; files close the quote.

Each Tab on a path lists that directory: one NameNode request per listing page (1,000 entries by default), with the
current session's secrets and settings. A huge directory, or a cluster that doesn't answer, makes Tab wait for the
listing or for httpfs's timeout; the shell then just offers nothing.

## Performance notes

- Each file read costs one NameNode request (`OPEN`, answered by a redirect to a DataNode); later reads of the same
  file go straight to the DataNode. Files found by a glob or listing are opened without asking for their status
  again (DuckDB's CSV reader is the exception: it opens files by name, which costs a `GETFILESTATUS` per open). Listed
  files carry an `etag`, so with `SET parquet_metadata_cache = true` a repeated query over the same Parquet files
  sends only the listing. `test/sql/integration/webhdfs_requests.test` pins these request counts.
- Files up to `webhdfs_read_buffer_size` are fetched in one request and kept in a small cache (`webhdfs_small_file_cache_size`, 64 MiB per
  cluster by default, keyed by identity, path and version tag; not counted against `memory_limit`), so a reader that opens a file twice (DuckDB's Parquet reader does)
  reads it once. A read near the end of a larger file fetches its tail, where Parquet keeps its footer.
- Listings are paged and run in parallel (`webhdfs_list_parallelism`); request counts matter more than bandwidth for
  many small files. See [test/bench](test/bench/README.md) for numbers.
- DuckDB's external file cache treats `webhdfs://` paths as local files, so it caches them only with
  `SET cache_local_files = true`. WebHDFS files carry a version tag of their inode id (`fileId`), modification time and
  length, so cached data and Parquet metadata are invalidated when a file is replaced, even by one of the same length
  with its old modification time kept (`distcp -pt`), when the server reports `fileId` (HDFS NameNodes do; without
  it, only the modification time and length tell versions apart), under `validate_external_file_cache = 'VALIDATE_ALL'` (the
  default).
  `VALIDATE_REMOTE` treats `webhdfs://` as local and never revalidates it; `NO_VALIDATION` never does either.

## Limitations

- Files can't be written at arbitrary offsets or truncated (HDFS is append-only). A chunk that fails to `CREATE` or
  `APPEND` is not re-sent (the server may already have applied it); the write fails and the handle refuses further
  writes.
- A write that fails, or whose handle is destroyed while an error unwinds, deletes the file it created. That includes a
  final `Close()` that fails after the last chunk was sent, which DuckDB itself treats as an unknown outcome. (A handle
  closed normally, or dropped without an error, commits, as local files do.) With `COPY ... TO` over an existing
  file, DuckDB writes a temporary file and renames it over the target (an atomic `RENAME` with
  `renameoptions=OVERWRITE`; HttpFS gateways that lack the option fall back to delete-then-rename), so the original
  survives a failure. With `USE_TMP_FILE false`, a failed overwrite loses the original: once the `CREATE` that
  replaces it has succeeded, the abort deletes the file, and a `CREATE` whose DataNode upload failed can leave an
  incomplete file. An aborted append to an existing file is not rolled back.
- Partition filters don't reduce listing (see above).
- Writes buffer up to `webhdfs_write_chunk_size` per open file; `PARTITION_BY` with many open partitions multiplies
  that.
- Not tested yet: `swebhdfs://` against a TLS cluster, a real Apache Knox (gateway paths and `http` secret headers
  were only checked against a stand-in server), Kerberos with a gateway that authenticates every request instead of
  issuing tokens, and the loadable extension on macOS and Windows (on Linux, CI tests it in a stock DuckDB 2.0.0
  with httpfs from DuckDB's extension repository).
- No metadata-changing functions (`SETPERMISSION`, `SETOWNER`, quotas, snapshots).
- Builds for Linux, macOS and Windows, not for WebAssembly: a browser's fetch follows redirects itself, but the
  extension must handle DataNode redirects (address mapping, request bodies), and Kerberos needs `dlopen`. On
  Windows, autocomplete of `webhdfs://` directories relies on a workaround for the shell's path separator (`/`, not
  `\`) that only CI's Windows build exercises.
- An `http` secret's extra headers and bearer token are sent with the DataNode requests a NameNode redirects to, as
  well as to the NameNode or gateway. Where DataNodes must not see a credential, give it in a `webhdfs` secret
  (`AUTH`, `DELEGATION_TOKEN`) instead: those are sent to the NameNode only.
- With `enable_external_access = false`, `allowed_directories` and `allowed_paths` are checked against the
  `webhdfs://` URL, not against the DataNode a NameNode redirects to. A NameNode, or on plain `webhdfs://` anyone who
  can rewrite its responses, can send a read or write to any host, internal ones included. With Kerberos and
  `spnego_every_request`, a ticket for that host is requested from the KDC. `swebhdfs://` rules out the man in the
  middle, not a hostile NameNode, so these settings are no sandbox against the cluster.
- As in DuckDB, an allowed directory itself (`webhdfs://nn/data` for `allowed_directories = ['webhdfs://nn/data/']`)
  is accessible to calls on a directory: listing, creating or removing it, and the metadata functions, which check
  every path as a directory. Opening it as a file is refused.
- A request whose response is lost is retried by DuckDB. For `DELETE`, the retry then reports "no such file" for a
  file that was deleted; for a write that must not overwrite (`CREATE` without overwrite), it reports that the file
  already exists. Neither leaves wrong data, but the statement fails.
- A write handle that is dropped without `Close()` commits from its destructor, and a failure there can't be
  reported: DuckDB's `EXPORT DATABASE` drops `schema.sql` and `load.sql` that way, so a failed commit of either goes
  unnoticed. `COPY ... TO` always closes its files.
- `SET redact_http_logs = false` puts full WebHDFS request URLs in `duckdb_logs`, including `delegation=` tokens
  and `user.name=`.

## Building

Targets DuckDB v2.0.0 (the `duckdb` submodule). httpfs is built alongside, from the commit DuckDB v2.0.0 pins, for the
test runner and the bundled shell only: the extension itself doesn't link it.

```sh
git submodule update --init --recursive
GEN=ninja make            # needs CMake, a C++17 compiler, and libcurl + OpenSSL headers (for httpfs)
```

The CI distribution build gets curl and OpenSSL from vcpkg (`vcpkg.json`). Artifacts:

- `build/release/duckdb`: shell with webhdfs, httpfs and autocomplete linked in
- `build/release/extension/webhdfs/webhdfs.duckdb_extension`: the loadable extension (about 13 MB, almost all of it
  the parts of DuckDB core it links statically, like every C++ extension; Linux release builds strip its symbol
  table)
- `build/release/test/unittest`: the test runner

Builds are stamped with the DuckDB release the submodule is checked out at (`DUCKDB_VERSION`, from its git tag), so
the loadable extension loads in that release (with `allow_unsigned_extensions`). DuckDB's CMake doesn't read git
tags itself: a build without `DUCKDB_VERSION` is a dev build that only loads in a DuckDB built from the same commit.

## Testing

```sh
make test_unit                          # URL, glob, Hadoop config and JSON tests (no DuckDB build needed)
build/release/test/unittest "test/sql/*"  # SQL tests; cluster tests are skipped without a cluster
test/scripts/run_webhdfs_tests.sh       # Docker clusters + integration tests (needs Docker)
test/scripts/run_kerberos_tests.sh      # KDC + Kerberized cluster + Kerberos tests (needs Docker and MIT Kerberos)
make test_loadable                      # the release build in a stock DuckDB from pip, against the Docker cluster
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

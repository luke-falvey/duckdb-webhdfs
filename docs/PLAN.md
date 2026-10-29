# Plan: a WebHDFS filesystem for DuckDB

Status: implemented (see "Implementation status" at the end). Targets DuckDB
v2.0.0, the same version as `duckdb-hdfs` and this repo's CI.

## Goal

A `webhdfs` extension that reads and writes HDFS through its REST API,
[WebHDFS](https://hadoop.apache.org/docs/stable/hadoop-project-dist/hadoop-hdfs/WebHDFS.html),
with every HTTP request sent through DuckDB's `httpfs` extension.

```sql
INSTALL httpfs; LOAD httpfs;   -- loaded automatically on first use if autoloading is on
LOAD webhdfs;

SELECT * FROM 'webhdfs://namenode:9870/data/events/*.parquet';
SELECT * FROM read_csv('swebhdfs://namenode:9871/data/users.csv');
COPY (SELECT * FROM t) TO 'webhdfs://namenode:9870/out/t.parquet' (FORMAT parquet);
SELECT * FROM webhdfs_ls('webhdfs://namenode:9870/data/**');
```

Why it is worth having next to `duckdb-hdfs`:

- **No native client.** The hdfs extension builds libhdfs3, protobuf, gsasl and
  krb5 through vcpkg. This extension has no third-party dependencies of its own:
  HTTP, TLS, proxies, retries and connection pooling all come from httpfs.
- **It works where RPC can't reach.** It goes through firewalls, HTTP proxies,
  Apache Knox, and the HttpFS gateway, and it runs on any platform httpfs
  supports (Windows, possibly WASM later).
- **The user model is the same.** Users get the same globbing, listing functions,
  and settings as the hdfs extension, under a `webhdfs_` prefix.

**Rule: no code is shared with duckdb-hdfs.** This extension is written from
scratch. duckdb-hdfs is a reference for behaviour and for the user-facing
interface only: setting names, function columns, glob semantics. No source files
are copied, vendored, submoduled or pulled from a common repo. The two
extensions can then change on their own schedules, and neither ties the other to
its build. Where this plan says "as in duckdb-hdfs", it means the same behaviour,
implemented here.

## 1. How httpfs supplies HTTP

DuckDB v2.0.0 core defines an abstract HTTP layer in
`duckdb/main/http/http_util.hpp`:

- `HTTPUtil`: the provider, which you get with `HTTPUtil::Get(db)`. Core's
  default has `GetName() == "none"` and returns a null client, so requests fail.
- When httpfs loads, it installs its curl-based `HTTPFSUtil` with
  `DBConfig::SetHTTPUtil`. From then on, `HTTPUtil::Get(db)` returns the httpfs
  implementation.
- Requests are built as `GetRequestInfo`, `PutRequestInfo`, `PostRequestInfo`,
  `DeleteRequestInfo` and `HeadRequestInfo`, and sent with
  `HTTPUtil::Request(request[, client])`. Retries with backoff, retry budgets,
  logging, the external-access check, and client pooling (the transport manager)
  all happen inside that call.
- `HTTPUtil::InitializeParameters(opener, info)` returns the httpfs
  `HTTPFSParams`. It is already filled from the httpfs settings (`http_timeout`,
  `http_retries`, `http_proxy*`, `http_keep_alive`, `extra_http_headers`, CA
  certificates, …) and from any `TYPE http` secret whose scope matches
  `info.file_path`, which covers bearer tokens and extra headers.

So the extension uses **only core headers** and never links httpfs. That
matters because a loadable extension can't call another extension's symbols.
Any dependency on httpfs at compile time would break the loadable build.

### Making sure httpfs is there

DuckDB extensions can't declare a dependency on another extension in their
metadata. So webhdfs loads httpfs itself, in two steps.

**Step 1: when webhdfs loads, try to load httpfs (it may fail).**

- `LoadInternal` calls `ExtensionHelper::TryAutoLoadExtension(db, "httpfs")`.
- If httpfs isn't installed yet, that function installs it from the autoinstall
  repository and then loads it.
- It follows the user's `autoinstall_known_extensions` and
  `autoload_known_extensions` settings. Both are on by default in DuckDB v2.0.0.
- It is `noexcept` and returns false on failure, for example when offline or
  when autoloading is off. In that case `LOAD webhdfs` still succeeds.
- Loading one extension from inside another's load step is safe. DuckDB's
  `ExtensionManager::BeginLoad` takes a lock per extension, not a global one.
  `duckdb-iceberg` already loads its dependencies this way.
- Doing this when webhdfs loads also means httpfs's settings and secret types
  exist straight away. `SET http_timeout = …` and `CREATE SECRET (TYPE http …)`
  work right after `LOAD webhdfs`, before any WebHDFS path has been touched.

**Step 2: before every request, check that httpfs is there.**

One helper, `WebHdfsHTTP::Get(DatabaseInstance &)`, runs before every request:

1. If `HTTPUtil::Get(db).GetName() == "none"`, call `TryAutoLoadExtension`
   again. This covers sessions where step 1 failed but the cause has since gone
   away.
2. If the provider is still `"none"`, throw
   `MissingExtensionException("webhdfs requires the httpfs extension: INSTALL httpfs; LOAD httpfs;")`.

The provider is looked up on every request and never cached. So if a user runs
`LOAD httpfs` by hand after webhdfs, it is picked up at once.

In builds where httpfs is statically linked (the test runner and the bundled
shell, see §2), it is already loaded and both steps do nothing.

### Request rules

| Concern | Rule |
|---|---|
| Parameters | One `HTTPParams` per filesystem call, from `InitializeParameters(opener, {file_path = original webhdfs:// URL})`, so `http` secrets can be scoped to WebHDFS URLs. Set `access_path` to the `webhdfs://` URL so the file-access check sees the user's path. |
| Redirects | Set `follow_location = false` on NameNode calls, then read `Location` from the `307` response and make the DataNode call ourselves (see §3). |
| Errors | Set `try_request = true` so we see non-2xx bodies and can turn the Hadoop `RemoteException` JSON into a DuckDB exception (see §6). |
| Retries | Use httpfs retries for idempotent ops (GET, PUT `CREATE`, DELETE, `RENAME`, `MKDIRS`). For `POST APPEND` to a DataNode, set `retries = 0`: replaying it could duplicate data. |
| Client reuse | Keep one `unique_ptr<HTTPClient>` per open file handle, so sequential reads reuse a single keep-alive connection, and return it with `CloseClient` when the handle closes. Metadata calls use the shared pool. |

## 2. Source layout

All files are new and written for this repo. The left column only shows which
duckdb-hdfs file covers the same job, for orientation.

| Same job in duckdb-hdfs | duckdb-webhdfs | Notes |
|---|---|---|
| `hdfs_extension.cpp` | `webhdfs_extension.cpp` | Registers both schemes, settings, the `webhdfs` secret type, and the SQL functions. |
| `hdfs_bridge.{h,cpp}` (libhdfs3) | `webhdfs_client.{hpp,cpp}` | The REST layer: URL and operation building, auth query parameters, NameNode selection and failover, redirect handling, JSON → `WebHdfsEntry`, and error mapping. It is the only file that talks to `HTTPUtil`. |
| `hdfs_filesystem.{hpp,cpp}` | `webhdfs_filesystem.{hpp,cpp}` | `FileSystem` subclass: handles, extended open, extended listing, version tag, directory ops, and the filter-aware file list (§5c). |
| `hdfs_glob.{hpp,cpp}` | `webhdfs_glob.{hpp,cpp}` | Our own glob engine (§5), written to the same pattern syntax so users see the same behaviour. It is designed from the start to accept partition filters while it walks. |
| `hdfs_functions.{hpp,cpp}` | `webhdfs_functions.{hpp,cpp}` | `webhdfs_ls`, `webhdfs_stat` and `webhdfs_exists`, with the same columns and named parameters. |
| `hdfs_batch.hpp` | — | Not needed. WebHDFS has no batched multi-directory listing, so parallelism comes from concurrent requests (§5). |
| `hdfs_crypto.*` | — | Out of scope. Encryption zones are decrypted on the DataNode for WebHDFS reads. |
| — | `webhdfs_json.{hpp,cpp}` | Thin wrappers over the yyjson copy that ships in DuckDB core (`duckdb_yyjson`), which saves vendoring a JSON parser. If the loadable build can't resolve those symbols on some platform, vendor `yyjson.c` instead (it is one file, MIT licensed). |
| — | `webhdfs_url.{hpp,cpp}` | Parsing `webhdfs://` and `swebhdfs://`, percent-encoding paths, and building the REST URLs. Kept separate so it can be unit tested without a cluster. |

Build changes:

- Rename `waddle` to `webhdfs` in `CMakeLists.txt`, `extension_config.cmake`,
  the `Makefile`, CI, and tests. Delete the template's `waddle_*` sources and
  test.
- Drop the OpenSSL dependency, which leaves `vcpkg.json` with no dependencies.
- In `extension_config.cmake`, also load httpfs from Git, pinned to the commit
  that matches DuckDB v2.0.0, so the test runner and the static shell have it:

  ```cmake
  duckdb_extension_load(webhdfs SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR} LOAD_TESTS)
  duckdb_extension_load(httpfs
      GIT_URL https://github.com/duckdb/duckdb-httpfs
      GIT_TAG <commit matching v2.0.0>
      INCLUDE_DIR src/include)
  ```

  This is for tests and the bundled shell only. The `webhdfs` target itself does
  not link httpfs.
- Set up our own `.clang-format`/`.clang-tidy` files and a `test_unit` Makefile
  target that builds the C++ unit tests in `test/cpp`.

## 3. Mapping FileSystem calls to WebHDFS

`P` below is `<base>/webhdfs/v1<encoded path>`, where `<base>` is
`http(s)://host:port` plus an optional gateway prefix (§4).

| FileSystem method | WebHDFS call(s) |
|---|---|
| `OpenFile` (read) | `GETFILESTATUS` (size, mtime, type). It is skipped when `OpenFileExtended` already has size and mtime from a listing. |
| `Read(handle, buf, n, offset)` | `GET P?op=OPEN&offset=&length=` to the NameNode → `307` → `GET <Location>` on the DataNode, streamed into the caller's buffer through `content_handler`. Behind a gateway, the first response is already `200` with the data. |
| `GetFileSize`, `GetLastModifiedTime`, `GetVersionTag` | Values cached on the handle. The version tag is `"<mtime>:<length>"`, as in the hdfs extension, so the external file cache can validate reads without another request. |
| `OpenFile` (write) | Nothing yet. The file is created on the first flush (see Writes). |
| `Write` / `Close` | Buffer, then `PUT ?op=CREATE&overwrite=true` → `307` → `PUT <Location>` with the body. Later chunks: `POST ?op=APPEND` → `307` → `POST <Location>` with the body. |
| `FILE_FLAGS_APPEND` | Every flush is an `APPEND`. |
| `FileExists` / `DirectoryExists` | `GETFILESTATUS`. A `FileNotFoundException` means false. |
| `CreateDirectory[Extended]` | `PUT ?op=MKDIRS`. It returns `{"boolean":…}` and creates parents. To report "created by this call" for `CreateDirectoryExtended`, stat first. |
| `RemoveDirectory[Extended]`, `RemoveFile` | `DELETE ?op=DELETE&recursive=true|false`. |
| `MoveFile` | `PUT ?op=RENAME&destination=`. A `{"boolean":false}` reply has no reason attached, so stat the source and target to give a useful error. |
| `ListFilesExtended` | `GET ?op=LISTSTATUS_BATCH` (Hadoop 2.8+), paged with `startAfter` so huge directories stream. It falls back to `LISTSTATUS` on the first `UnsupportedOperationException` or `IllegalArgumentException` and remembers that per NameNode. Each entry fills `OpenFileInfo::extended_info` (type, size, mtime). |
| `Glob` | The hdfs glob engine (`GlobPlan`) driven by a parallel listing walk (§5). |
| `FileSync` | No-op. Data is durable when the file is closed. |
| `CanHandleFile` | Prefix `webhdfs://` or `swebhdfs://`. |

### Reads

- **Redirect handling.** We follow the `307` ourselves rather than letting curl
  do it. That lets us do two things:
  - Rewrite DataNode hosts (§4, `datanode_address_map`). This is needed for
    Docker, Kubernetes and NAT setups, and is the WebHDFS counterpart of the hdfs
    extension's `dfs.client.use.datanode.hostname` tip.
  - Avoid sending gateway or bearer credentials to DataNodes when they're not
    wanted.
- **Reusing the redirect.** A DataNode's WebHDFS handler reads any offset of the
  file, because it opens its own DFSClient. So each handle caches the DataNode
  `Location` from its first read. Later reads swap the `offset`/`length` query
  parameters and go straight to the DataNode: one round trip instead of two. On
  any error from that URL (403, an expired token, the DataNode is gone), drop the
  cache and retry once through the NameNode. Ship this behind the
  `webhdfs_reuse_redirects` setting, on by default, after testing it.
- **Read-ahead.** DuckDB's sequential readers (CSV, JSON) issue many small reads,
  and each one is a round trip. Each handle keeps a buffer (setting
  `webhdfs_read_buffer_size`, default 1 MiB): a short sequential read fetches the
  whole buffer. Positional reads that are large enough (Parquet column chunks) go
  straight into the caller's buffer.
- **Concurrency.** Positional `Read` must be thread-safe, because the Parquet
  reader shares a handle across threads. Give each read its own `HTTPClient`, or
  protect a small per-handle client pool with a lock. Guard the cached redirect
  and the read-ahead buffer with a mutex.
- **Short reads.** Check the number of bytes received against what was requested.
  If a body is cut off, retry once from the last byte received.

### Writes

HDFS is append-only, and `PutRequestInfo` takes a whole body rather than a
stream, so:

- Buffer writes on the handle up to `webhdfs_write_chunk_size` (default 64 MiB),
  using DuckDB's `Allocator`.
- The first flush, or `Close` if fewer bytes were written, sends `CREATE` with
  that chunk. Each later chunk sends `APPEND`. A file smaller than one chunk is
  therefore a single `CREATE`, which works even on clusters with append turned
  off.
- `CREATE` passes the optional `permission`, `replication` and `blocksize` taken
  from the `webhdfs` secret or settings.
- Because `APPEND` is a non-idempotent POST, it is never retried automatically.
  A failure leaves the file truncated and raises a clear error. DuckDB's
  `COPY … TO` writes then renames in some modes, so a partial file won't be
  mistaken for success.
- Close order: flush the remainder, then mark the handle closed. A destructor
  that hits an error swallows it, the same as in `HdfsFileHandle`.
- Later improvement: on HttpFS gateways, a single PUT with `data=true` and
  `Content-Type: application/octet-stream` saves the redirect round trip.

## 4. URLs, endpoints and authentication

### URL forms

- `webhdfs://host[:port]/path` uses HTTP, default port 9870.
- `swebhdfs://host[:port]/path` uses HTTPS, default port 9871.

These are Hadoop's own schemes, so URLs copied from `hadoop fs` work unchanged.
`hdfs://` is left to the hdfs extension.

Paths are percent-encoded segment by segment (space, `%`, `#`, `?`, non-ASCII).
`/` and the characters HDFS allows are kept as they are.

### The `webhdfs` secret

Connection settings live in a DuckDB secret with type `webhdfs`, scoped by URL
prefix. This follows the S3 model and keeps credentials out of SQL text.

```sql
CREATE SECRET prod_hdfs (
    TYPE webhdfs,
    SCOPE 'webhdfs://mycluster',
    USER_NAME 'analytics',            -- simple auth: user.name=
    PROXY_USER 'alice',               -- doas=
    DELEGATION_TOKEN '...',           -- delegation=
    NAMENODES ['nn1.example:9870', 'nn2.example:9870'],   -- HA for a logical name
    GATEWAY_PATH '/gateway/default',  -- Knox: prefix before /webhdfs/v1
    DATANODE_ADDRESS_MAP MAP {'dn-0.internal:9864': 'localhost:9864'},
    USE_SSL true                      -- override the scheme's default
);
```

Resolution order for each request:

1. The matching `webhdfs` secret.
2. Settings: `webhdfs_user_name`.
3. The environment: `HADOOP_USER_NAME`, then the OS user.

TLS options (CA bundle, certificate verification), proxies and extra headers
come from httpfs settings and `http` secrets. We don't add duplicates of these.

### Authentication modes (phased)

| Mode | How | Phase |
|---|---|---|
| Simple (`user.name`, `doas`) | Query parameters. | 1 |
| Delegation token | `delegation=` query parameter. The NameNode also passes it on in the DataNode redirect. | 1 |
| Gateway (Knox / HttpFS with Basic or Bearer auth) | Through a `TYPE http` secret: `bearer_token` or `extra_http_headers` with `Authorization`. This works without any code in this extension. | 1 (document and test) |
| Kerberos (SPNEGO) | See below. | 3 |

Kerberos (SPNEGO): the extension builds the `Authorization: Negotiate` token
itself with GSSAPI, loaded with `dlopen` from the system's Kerberos library
(`libgssapi_krb5` on Linux, `GSS.framework` on macOS, SSPI on Windows). Nothing
is built through vcpkg, and platforms without Kerberos still load the extension.
The flow:

1. Call `GETDELEGATIONTOKEN` once per NameNode with SPNEGO.
2. Use the returned token as `delegation=` on every later request. That avoids a
   GSS handshake per request and also covers DataNode redirects.
3. Renew the token before it expires (`RENEWDELEGATIONTOKEN`) and cancel it when
   the database shuts down.

The `hadoop.auth` cookie is a fallback for gateways that don't issue tokens.

### NameNode selection and high availability

- **A plain `host:port` authority** is one NameNode.
- **A logical authority** (`webhdfs://mycluster/…`) gets its NameNode list from:
  1. the `NAMENODES` secret field, or
  2. `hdfs-site.xml` under `HADOOP_CONF_DIR` or `HADOOP_HOME/etc/hadoop`:
     `dfs.ha.namenodes.<ns>` and `dfs.namenode.http-address.<ns>.<nn>`, or the
     `https-address` keys for `swebhdfs`.

  Phase 2 adds a small XML reader for those keys only.
- **Failover.** Remember the last NameNode that answered as active, per
  authority. On a connection failure, or a `RemoteException` of
  `StandbyException` or `ObserverRetryOnActiveException`, try the next one, up to
  one pass over the list. `RetriableException` and `SafeModeException` are
  retried after backoff.

## 5. Listing, globbing and SQL functions

- **Glob engine (`webhdfs_glob`).** Written here, to the same user-facing syntax
  as duckdb-hdfs:
  - DuckDB's `*`, `?`, `[abc]`, `[!abc]` and `\` escapes
  - `**` as a whole path component meaning any depth
  - Hadoop-style `{a,b}` alternation
  - hidden-entry rules: names starting with `_` or `.` are skipped unless the
    pattern names them
  - `skip_permission_errors`

  The pattern compiles to a list of components. The walk lists only directories
  the pattern can still match. Each component also records whether it is a
  `key=value` partition level, which is what §5c uses for pruning.
- **`WebHdfsListStream`.** A background walk has a
  bounded queue of pending directories and up to `webhdfs_list_parallelism`
  `LISTSTATUS_BATCH` requests in flight on worker threads. Entries stream to
  `Next()` in completion order, and `LISTSTATUS_BATCH` pages are queued as
  continuation items. Retry rules are the same: a connection failure retries
  transparently only before any entry has been handed out.
- **Settings.** The names match the hdfs extension's, with the prefix swapped:

  | Setting | Default |
  |---|---|
  | `webhdfs_list_parallelism` | 16 |
  | `webhdfs_include_hidden` | false |
  | `webhdfs_skip_permission_errors` | false |
  | `webhdfs_read_buffer_size` | 1 MiB |
  | `webhdfs_write_chunk_size` | 64 MiB |
  | `webhdfs_reuse_redirects` | true |
  | `webhdfs_user_name` | unset |
  | `webhdfs_partition_pruning` | true (§5c) |

- **SQL functions.** `webhdfs_ls(url, include_hidden :=, skip_permission_errors :=)`,
  `webhdfs_stat(url)` and `webhdfs_exists(url)` return the same columns as the
  hdfs versions: `path`, `name`, `type`, `size`, `owner`, `group`,
  `permissions`, `mode`, `replication`, `block_size`, `last_modified`,
  `last_accessed`. WebHDFS gives the `permission` field as an octal string, which
  we parse into `mode`.

## 5a. Multiple NameNodes

"Multiple NameNodes" covers two cases, and the extension handles both.

**Several clusters in one session.** Every URL carries its own authority, so one
query can join `webhdfs://prod-nn:9870/...` with `swebhdfs://dr-cluster/...`.

- The filesystem keeps a map from authority to `WebHdfsEndpoint`, created on
  first use and never removed, the same pattern as `HdfsFileSystem::connections`.
  An endpoint holds:
  - the resolved NameNode list and which one is active
  - the base URL and gateway prefix
  - the auth state (user, delegation token and its expiry)
  - capability flags (does it support `LISTSTATUS_BATCH`)
  - the DataNode address map
- Each authority picks up its own `webhdfs` secret by scope, so clusters can use
  different users, tokens and authentication modes in the same query.
- The `http` secrets and settings from httpfs (proxy, CA bundle, headers) are
  also matched per URL, so each cluster can have its own TLS and proxy settings.
- `webhdfs_namenodes()` (below) shows what each authority resolved to. That makes
  multi-cluster setups possible to debug.

**HA NameNode pairs or groups behind one logical name.** This is the failover
described in §4: one logical authority maps to an ordered NameNode list, from
the secret's `NAMENODES` or from `hdfs-site.xml`.

- Standby and observer replies, and connection failures, rotate to the next
  NameNode.
- The NameNode that answered as active is remembered per endpoint, behind a
  mutex. Parallel listing workers then don't each rediscover it.
- Observer reads (Hadoop 3.3+): later, optionally send read-only operations to
  observer NameNodes listed in `OBSERVERS`. Writes and `msync` always go to the
  active one. This is out of scope for the first release.

## 5b. Metadata functions

These all go through the same endpoint, auth and error machinery as the
filesystem. Each takes a `webhdfs://` URL, and the table functions return one
row per item. They are grouped by milestone.

| Function | WebHDFS op | Returns | Milestone |
|---|---|---|---|
| `webhdfs_ls(url, include_hidden :=, skip_permission_errors :=)` | `LISTSTATUS[_BATCH]` + glob walk | One row per entry. Columns, as in `hdfs_ls`: path, name, type, size, owner, group, permissions, mode, replication, block_size, last_modified, last_accessed. Adds `storage_policy`, `ec_policy`, `file_id`, `children_num` because FileStatus carries them. | 2 |
| `webhdfs_stat(url)` | `GETFILESTATUS` | The same columns, one row. | 2 |
| `webhdfs_exists(url)` | `GETFILESTATUS` | Scalar `BOOLEAN`. | 2 |
| `webhdfs_content_summary(url)` | `GETCONTENTSUMMARY` | Recursive totals for a directory: `length`, `file_count`, `directory_count`, `space_consumed`, `quota`, `space_quota`, `erasure_coding_policy`. One row, computed by the NameNode. | 2 |
| `webhdfs_quota_usage(url)` | `GETQUOTAUSAGE` | Namespace and space quota and usage, plus per-storage-type quotas as a `MAP`. | 4 |
| `webhdfs_block_locations(url, offset :=, length :=)` | `GETFILEBLOCKLOCATIONS`, falling back to `GET_BLOCK_LOCATIONS` | One row per block: `offset`, `length`, `hosts` (LIST), `names`, `topology_paths`, `storage_types`, `corrupt`. | 4 |
| `webhdfs_checksum(url)` | `GETFILECHECKSUM` | `algorithm`, `length`, `bytes` (hex). Useful for comparing copies across clusters. | 4 |
| `webhdfs_acl(url)` | `GETACLSTATUS` | owner, group, sticky bit, permission, and `entries` as a `LIST(VARCHAR)`. | 4 |
| `webhdfs_xattrs(url, names := [...])` | `GETXATTRS` | One row per xattr: `name`, `value` (BLOB). | 4 |
| `webhdfs_storage_policy(url)` | `GETSTORAGEPOLICY` | The policy name, and storage types for creation and replication. | 4 |
| `webhdfs_snapshots(url)` | `GETSNAPSHOTLIST` / `GETSNAPSHOTTABLEDIRECTORYLIST` | Snapshots of a directory, or every snapshottable directory when given the root. | 5 |
| `webhdfs_home_directory(authority)` | `GETHOMEDIRECTORY` | Scalar `VARCHAR`. | 4 |
| `webhdfs_trash_root(url)` | `GETTRASHROOT` | Scalar `VARCHAR`. | 4 |
| `webhdfs_server_defaults(authority)` | `GETSERVERDEFAULTS` | One row: block size, replication, checksum type, encryption key provider, … | 4 |
| `webhdfs_namenodes()` | none (local state) | One row per known authority and NameNode: `authority`, `namenode_url`, `state` (`active`, `standby`, `unknown`), `last_error`, `auth_mode`, `supports_batched_listing`. | 4 |

Design notes:

- **Common scaffolding.** The functions share one table-function scaffold
  within this extension. A JSON-to-row adapter per operation keeps each new
  function to about 50 lines. That matters because the list is long and the
  WebHDFS responses differ in shape.
- **Lateral calls.** Each function also accepts a column argument in a lateral
  join, for example
  `SELECT f.path, s.* FROM webhdfs_ls('…/*') f, webhdfs_content_summary(f.path) s`.
  So implement them as in-out table functions, or as table functions that take
  `LIST` input, rather than plain constant-parameter ones.
- **Versions and gateways.** Operations that older Hadoop versions or gateways
  reject (`GETQUOTAUSAGE`, `GETSNAPSHOTLIST`, `GETFILEBLOCKLOCATIONS`) raise a
  clear "not supported by this NameNode (Hadoop ≥ x.y required)" error, built
  from the mapped `UnsupportedOperationException` or `IllegalArgumentException`.
- **No writes.** All of these are read-only. Writing metadata (`SETPERMISSION`,
  `SETOWNER`, `SETXATTR`, `SETREPLICATION`, quotas, snapshots) is left out on
  purpose. It could follow later as scalar functions that need explicit opt-in.

## 5c. Partitioned (Hive-style) data

### Reading

DuckDB core handles Hive partitioning. webhdfs only has to glob and list
correctly:

```sql
SELECT * FROM read_parquet('webhdfs://nn:9870/warehouse/events/**/*.parquet',
                           hive_partitioning = true)
WHERE year = 2024 AND month = 10;
```

1. webhdfs expands the glob, listing only the directories the pattern can reach.
2. DuckDB parses `key=value` folders into columns. `hive_types` and
   `hive_types_autocast` work as usual.
3. DuckDB's `MultiFileList::ComplexFilterPushdown` drops files whose partition
   values fail the `WHERE` clause, before opening them. Dropped files cost no
   WebHDFS requests.
4. The hidden-entry default skips the leftovers Spark and Hive write (`_SUCCESS`,
   `_temporary/`, `.crc` files), so nothing extra appears as a file or a
   partition.

Because listings carry size and modification time in `extended_info`, opening
the files that survive needs no extra `GETFILESTATUS`.

### Pruning partitions while listing

**The gap.** In DuckDB v2.0.0 the default `ComplexFilterPushdown` calls
`GetAllFiles()`, which expands the whole glob first and filters afterwards.
With `**`, or `*` at partition levels, every partition directory is listed, at
one NameNode request each, even when the query wants one month. On a table with
years of daily partitions, that is thousands of requests before any data is
read.

**The fix.** Core lets a filesystem supply its own file list, through
`FileSystem::GlobFilesExtended` and `SupportsGlobExtended`; httpfs uses this for
S3. webhdfs returns a `WebHdfsMultiFileList`, a `LazyMultiFileList` subclass.
It overrides `ComplexFilterPushdown` and `DynamicFilterPushdown` so the filters
are applied during the walk:

- **When pushdown runs**, the list hasn't started walking yet. It keeps the
  filter expressions that refer only to partition columns (via `column_map`,
  the same input core's `HivePartitioning` uses) and returns a new, unexpanded
  list that carries them.
- **During the walk**, each time it enters a directory named `key=value`, it
  knows the values of every partition column above that point. It evaluates each
  filter conjunct whose columns are all known, with the same value parsing as
  core (`HivePartitioning::GetValue`, which also handles `NULL` and
  `__HIVE_DEFAULT_PARTITION__`). If a conjunct is false, it skips the whole
  subtree without listing it. Conjuncts whose columns aren't known yet wait for
  deeper levels.
- **Equality shortcut.** If a conjunct is `key = constant`, or
  `key IN (constants)`, and the next pattern component is a wildcard, the walk
  goes straight to `key=<escaped value>` with one `GETFILESTATUS` per value. It
  doesn't list the parent at all. A missing folder simply means no files.
- **Safety.** The walk only prunes by partition columns. Core still runs its own
  filter over the files that come back, so a pruning bug can only cost extra
  requests, never wrong results, as long as pruning never drops a file that
  should match. Tests (§7) compare pruned and unpruned file lists on the same
  fixtures to check exactly that.
- **Display and cardinality.** `GetDisplayFileList` and `GetTotalFileCount`
  expand lazily, as the base class does, so `EXPLAIN` and progress reporting
  keep working.

For `WHERE year = 2024 AND month = 10` over `year=*/month=*/day=*`, the walk
makes one request for `year=2024/month=10/`, then one listing per day under it,
instead of a listing for every directory in the table.

Until this lands (milestone 5), the workaround from the duckdb-hdfs README
applies: put fixed folder names in the path where possible, for example
`…/year=2024/month=10/*/*.parquet`.

### Writing

```sql
COPY t TO 'webhdfs://nn:9870/out' (FORMAT parquet, PARTITION_BY (year, month));
```

This uses only the filesystem operations from §3, so it needs no special code:

- `DirectoryExists` and `CreateDirectoryExtended` (`MKDIRS`) for each
  `key=value` folder
- `FileExists` and `RemoveFile` / `RemoveDirectoryExtended` for
  `OVERWRITE_OR_IGNORE` and `OVERWRITE`
- one write handle per open partition (`CREATE`, then `APPEND`)

Memory is the concern: each open partition holds a write buffer (§3 Writes and
§9). `webhdfs_write_chunk_size` should be documented next to DuckDB's
`partitioned_write_max_open_files`.

## 6. Errors

WebHDFS errors come back as
`{"RemoteException":{"exception":"…","javaClassName":"…","message":"…"}}`,
alongside a 4xx/5xx status. Map them as follows:

| Hadoop exception | DuckDB behaviour |
|---|---|
| `FileNotFoundException` | `IOException` "No files found that match the pattern" / "File … does not exist". `FileExists` returns false. |
| `AccessControlException`, `SecurityException`, HTTP 401 | `PermissionException`. Used for pruning when `skip_permission_errors` is on. |
| `FileAlreadyExistsException` | `IOException` (when CREATE has `overwrite=false`). |
| `StandbyException`, `ObserverRetryOnActiveException` | Fail over (§4). |
| `RetriableException`, `SafeModeException`, HTTP 503 | Retry with backoff, then `IOException`. |
| Anything else, or a body that isn't JSON | `IOException` with the status, the exception class and the message, plus the operation and the redacted URL. |

Redact `delegation=` and `user.name=` from every URL that appears in error
messages and in the HTTP logger. httpfs redacts headers, not query strings, so
`HTTPParams::logger` alone isn't enough.

## 7. Testing

- **Unit tests** (`test/cpp`, `make test_unit`):
  - URL parsing and encoding
  - REST URL building
  - `FileStatus` and `RemoteException` JSON parsing
  - error mapping
  - `hdfs-site.xml` HA resolution
  - DataNode address mapping
  - glob engine tests, written here (pattern parsing, matching, hidden rules,
    `{a,b}`, `**`)
  - partition pruning: for a fake directory tree and a set of filters, the
    pruned walk returns the same files as a full walk followed by core's filter,
    and lists fewer directories
- **SQL tests without a cluster** (`test/sql/webhdfs_no_cluster.test`):
  - scheme handling
  - settings and secret creation
  - a friendly error when the NameNode is unreachable
  - a missing-httpfs test that runs on a loadable-only build, where
    `autoload_known_extensions=false` gives the "requires httpfs" message
  - `LOAD webhdfs` loads httpfs too, so `SET http_timeout` works right after it
- **Integration tests** (`test/sql/webhdfs.test`, gated by
  `require-env WEBHDFS_TEST_RUNNING`). This repo gets its own `test/docker`: a
  single-node Hadoop cluster built from the official `apache/hadoop` image, with
  the NameNode HTTP port 9870 and the DataNode HTTP port 9864 published. The
  DataNode is advertised as `localhost`, and a second run uses
  `DATANODE_ADDRESS_MAP` instead. Scripts: `test/scripts/webhdfs_up.sh` (start
  and seed fixtures), `webhdfs_down.sh` and `run_webhdfs_tests.sh`. Cover:
  - reading CSV, Parquet and JSON
  - `COPY` with `PARTITION_BY`, then reading it back with `hive_partitioning`
  - Hive reads with partition filters: the results match an unfiltered read, and
    the HTTP log (`duckdb_logs` with the HTTP log type) shows fewer listing
    requests than the number of partition directories
  - Spark-style output with `_SUCCESS` and `_temporary/` present
  - multi-chunk writes, which exercise `APPEND`
  - glob, `ls` and `stat`
  - rename, delete and `mkdir`
  - permissions, as a second user
  - hidden files
  - paths with spaces and non-ASCII characters
- **Gateway test.** Add an HttpFS server container (port 14000) to the same
  compose file, to cover the no-redirect path.
- **HA test.** Point the secret's `NAMENODES` list at a dead address followed by
  the live one, to check failover without a real HA cluster.
- **Multi-cluster test.** Run a second single-node cluster in the same compose
  project, on shifted ports, and query both in one statement with different
  secrets.
- **Metadata function tests.** Check each function against the Docker cluster.
  Assert exact values where the fixture makes them deterministic (sizes, counts,
  quotas we set with `hdfs dfsadmin -setQuota` in `webhdfs_up.sh`), and only the
  shape and types otherwise.
- **Kerberos (milestone 6).** A separate `test/docker-kerberos` in this repo: a
  KDC container plus a Kerberized single-node cluster with SPNEGO on the
  NameNode and DataNode HTTP endpoints.
- **CI.** Rename `extension_name` in `MainDistributionPipeline.yml`. Add a Linux
  job that runs the Docker integration tests.

## 8. Milestones

1. **Skeleton and reads.** Rename the template. Add the two-step httpfs loading
   (§1),
   URL parsing, the REST client with simple auth, `GETFILESTATUS`, `OPEN` with
   the two-step redirect, and the read-ahead buffer. Register the filesystem.
   - Done when: `SELECT * FROM 'webhdfs://localhost:9870/x.parquet'` works
     against the Docker cluster; no-cluster tests and unit tests run in CI.
2. **Listing and globbing.** `LISTSTATUS[_BATCH]`, extended info, our glob
   engine with the parallel walk, `ListFilesExtended`, the `webhdfs_ls`, `_stat`
   and `_exists` functions, and the hidden/permission settings.
   Also `webhdfs_content_summary`.
   - Done when: glob, listing and hidden-file tests pass, and Hive-partitioned
     Parquet reads with `hive_partitioning = true` return correct results (core
     filters after a full listing).
3. **Writes and directory ops.** `CREATE`/`APPEND` chunking, `MKDIRS`, `DELETE`,
   `RENAME`, and the `*Extended` variants.
   - Done when: `COPY … TO` (single file and `PARTITION_BY`) round-trips.
4. **Secrets, multiple NameNodes and metadata.** The `webhdfs` secret type,
   delegation tokens, `doas`, gateway prefix, DataNode address map, per-authority
   endpoints (§5a), NameNode failover, and `hdfs-site.xml` HA resolution. Add the
   metadata functions marked milestone 4 in §5b, including `webhdfs_namenodes()`.
   Document Knox and HttpFS use through `http` secrets.
   - Done when: one query reads from two Docker clusters with different users,
     and failover passes with a dead NameNode listed first.
5. **Performance.**
   - Partition pruning while listing (§5c): `GlobFilesExtended`, then
     `WebHdfsMultiFileList` with filter and dynamic-filter pushdown, then the
     equality shortcut.
   - Redirect reuse and per-handle client reuse.
   - Tuning the read-ahead and parallelism defaults.
   - A benchmark in `test/bench` that reports wall time and NameNode request
     counts. It includes a partitioned table with thousands of folders, queried
     with and without partition filters.
   - Done when: a filtered query on the benchmark table lists only the matching
     partitions, and the pruning tests in §7 pass.
6. **Kerberos/SPNEGO.** GSSAPI token generation, the delegation-token bootstrap
   and renewal, and the Kerberos Docker tests.
7. **README.** A user-facing README: what works, what doesn't, configuration,
   partitioned data, performance notes, building, and testing.

## 9. Risks and open questions

- **yyjson symbol visibility** for the loadable build on macOS and Windows. This
  needs checking early in milestone 1. The fallback is to vendor it.
- **HTTP client limits.** Is httpfs's `HTTPClient` safe to use from several
  threads for parallel listing? The plan avoids depending on that: one client
  per worker or per request.
- **Clusters with append turned off** (`dfs.support.append=false`) can only take
  files up to one write chunk. That calls for a clear error, and perhaps a
  "spool to a local temp file, then one `CREATE`" mode as a fallback.
- **Memory.** A whole write chunk is held in memory for each open writer.
  `PARTITION_BY` with many open partitions multiplies that, so the default chunk
  size may need to drop, or be capped by the number of open writers.
- **Autoloading.** DuckDB can't autoload `webhdfs` for `webhdfs://` paths,
  because core's prefix → extension table only covers known extensions. Users
  must run `LOAD webhdfs` (or `INSTALL` it from a community repo). The httpfs
  side autoloads.
- **Keeping behaviour in step with duckdb-hdfs.** Code is not shared (see the
  rule at the top), so the two extensions can drift in glob or hidden-file
  behaviour. Our own tests cover the behaviour users rely on: patterns, hidden
  entries, function columns. Differences are listed in the README.
- **Partition-pruning API stability.** `GlobFilesExtended` and the
  `MultiFileList` pushdown hooks are newer core APIs and may change between
  DuckDB releases. Keep that code in one file (`webhdfs_multi_file_list.cpp`)
  behind a setting (`webhdfs_partition_pruning`, default true) that falls back
  to the plain `Glob`.
- **httpfs loading when webhdfs loads.** If a user has
  `autoinstall_known_extensions` on but no network, `LOAD webhdfs` tries an
  install and waits for it to time out before succeeding. If that proves
  annoying, change step 1 to load only (`TryAutoLoadAvailableExtension`) and
  leave installing to step 2, on the first actual request.

## 10. Implementation status

Everything in milestones 1–7 is implemented and tested against real Hadoop 3.4
clusters in Docker (`test/sql/integration`, `test/sql/kerberos`). Where the
implementation differs from this plan:

- **Pruning partitions while listing (§5c) is not possible in DuckDB v2.0.0.**
  `MultiFileReader::BindOptions` iterates every file of a scan at bind time
  whenever `hive_partitioning` is on, to check that all files have the same
  partition columns. So the whole tree is listed before the optimizer sees any
  filter, whatever `hive_types_autocast` is. The `GlobFilesExtended` list,
  pruner and pre-optimizer hook were built, confirmed to be bypassed this way,
  and removed. What remains:
  - a lazily expanded file list, so non-Hive scans start before a large walk
    finishes;
  - directories a pattern names literally (`year=2024/month={5,6}`) are
    visited without listing their parent. The README recommends naming
    partitions in the path; the benchmark shows 62 instead of 2,085 NameNode
    requests for one month of a 1,680-partition table.
  A DuckDB change that defers or skips the bind-time check would make the
  original design possible again.
- **httpfs is required specifically.** DuckDB core also has a built-in httplib
  HTTP client (used for extension installs). It lacks httpfs's settings,
  proxies and `http` secrets, so webhdfs only accepts httpfs's provider.
  Loading first tries an httpfs that is already available (linked or
  installed) and only then installs one, so an offline machine with httpfs
  installed doesn't fail on the install step.
- **Redirect reuse is per cluster, not per open file.** Redirects are keyed by
  identity, path and file version, because DuckDB's Parquet reader opens each
  file twice.
- **Snapshots** are two functions: `webhdfs_snapshots(dir)` and
  `webhdfs_snapshottable_directories(url)`.
- **Kerberos tokens** are fetched again after 12 hours or when rejected, but
  not cancelled on shutdown (they expire on their own). The Kerberos tests run
  an unprivileged MIT KDC on the host rather than in a container.
- **The no-httpfs test** works in the regular test runner, because the runner
  turns extension autoloading off.


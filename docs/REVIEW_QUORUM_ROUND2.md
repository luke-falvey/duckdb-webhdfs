<!-- Output of the DuckDB Review Quorum (.github/agents), round 2, run on 2026-11-20: 15 simulated reviewer lenses,
each run as an independent read-only subagent over main at 68d1947 (after PR #4). Consensus = raised by 3+ lenses,
or by 2 with the same trace and medium/high severity. Per-lens reviews were working notes and are not kept. -->
# Quorum round 2: duckdb-webhdfs after PR #4

Round 1 ([REVIEW_QUORUM.md](REVIEW_QUORUM.md)) is merged. This round looked for what it missed, what its fixes broke
or left half done, and new problems. The lenses are simulations of reviewers' priorities, not the people.

## Verdicts
- **Request changes (6):** mytherin, hannes, samansmink, dtenwolde, kryonix, taniabogatsch.
- **Approve with nits (9):** lnkuiper, carlopi, tishj, hawkfish, maxxen, pdet, tmonster, szarnyasg, dentiny.

All six "request changes" verdicts rest on one or two medium items, each listed below. No lens found a crash, data
loss on the normal write path, or a regression of a round-1 security fix.

## Consensus items (implemented)

**R2-1. `webhdfs_small_file_cache_size` accepted a session `SET` and ignored it**
(mytherin, dtenwolde, dentiny, tishj, pdet, maxxen, szarnyasg: 7; incomplete C1)
- It was registered with the default SESSION scope, but the cache reads only the global value. `SET ... = 0`
  succeeded and `current_setting()` showed 0 while the cache stayed at 64 MiB, and the no-cluster test asserted
  exactly that.
- Now: an `ExtensionOption` with `default_scope = GLOBAL` and `supports_session = false`. A plain `SET` is
  database-wide and `SET SESSION` is an error.
- An `ExtensionOptionInfo::OnSet` hook resizes every cluster's cache at once. Before, the new size reached a cluster
  only when a client was next built for it (dtenwolde).
- Tests: `webhdfs_no_cluster.test` (scope), `integration/webhdfs_requests.test` (the cache is dropped at `SET`
  time).

**R2-2. Stale write contract comment** (mytherin, lnkuiper, dentiny, taniabogatsch: 4; regression of W1's follow-up)
- The header said an abandoned writer is "aborted, never committed", but the destructor commits since 83f9bcc. Fixed.
- Also fixed in the same area (carlopi, unique, cheap and clearly right): a `NULL_IF_EXISTS` open that returned
  nullptr left a live writer behind, whose destructor sent `CREATE overwrite=false`. It is now closed first. No core
  caller passes that flag, so no SQL test reaches it.

**R2-3. `webhdfs_namenodes()` showed another session's auth and secret** (szarnyasg, samansmink, mytherin,
dtenwolde: 4; incomplete K1)
- `auth_mode`, `namenode_source` and `secret_name` were per authority, last writer wins. One connection could see
  another connection's secret name, and the values went stale after `DROP SECRET`.
- Now: they are resolved for the calling session when the function runs, and they are no longer stored on the
  endpoint.
- Test: `webhdfs_secret_persistent.test` (the columns follow `DROP SECRET`).

**R2-4. Request counts were claimed but never asserted** (tmonster, lnkuiper, pdet, dentiny, plus hawkfish on the
stale bench: 5)
- New `integration/webhdfs_requests.test` counts requests per WebHDFS operation from DuckDB's HTTP log:
  - a Parquet glob sends no GETFILESTATUS;
  - `webhdfs_ls` of a directory sends one GETFILESTATUS;
  - a cached small file costs no DataNode request;
  - with the cache off it costs one;
  - with the Parquet metadata cache on, a repeated scan sends only the listing (R2-8).
- The bench README now says its numbers predate both rounds and are single runs.

**R2-5. Autocomplete's Windows separator guard changed routing on every platform** (mytherin, samansmink, maxxen,
dtenwolde, carlopi: 5; new in PR #4)
- The thread-local `CanHandleFile` override is now compiled only on Windows, where it is needed.
- The real fix is upstream: autocomplete should call `PathSeparator` on the listed directory, not the bare entry
  name (`autocomplete_extension.cpp:306`). See O6.

**R2-6. Glob file order depended on timing** (kryonix with a full trace, mytherin on `Glob()` vs walk order: 2;
deferred in round 1, now with a concrete bug)
- The walk sorted children by bare name, so `a/` came before `a-b/`. Core's `GlobMultiFileList` sorts each batch by
  full path, where `a-b/` sorts before `a/`, and batch boundaries depend on how far the prefetch has got.
- So the file order, including the first file the schema is bound from, could change between runs.
- Now: a directory sorts as `name/`, so the depth-first walk yields full paths in byte order and core's per-batch
  sort changes nothing. Eager `Glob()` agrees.
- Tests:
  - unit test `walk_order`;
  - a new `/test/order` fixture (`a/`, `a-b/`, `a.csv`) globbed with list parallelism 1 and 16;
  - the `read_csv` filename order.

**R2-7. Access checks ran after Kerberos and bypassed `disabled_filesystems`** (hannes, mytherin: 2, same fix
location, medium)
- The `webhdfs_*` functions reach `WebHdfsFileSystem::CreateClient` without the virtual file system, so
  `SET disabled_filesystems = 'WebHdfsFileSystem'` did not stop them.
- With `enable_external_access = false`, Kerberos still read the ticket cache and asked the KDC for a service ticket
  (a DNS and KDC side channel) before `HTTPUtil` refused the request.
- `CreateClient` now checks both before the secret lookup, `SiteConfig()` and GSSAPI.
- Tests: `webhdfs_access_control.test` and `webhdfs_disabled_filesystem.test`.

## Unique flags adopted (cheap and clearly right)
- **lnkuiper: listed files carry no `etag`.** Parquet's metadata cache compares the cached version tag with
  `extended_info["etag"]`, so under the default `VALIDATE_ALL` it never validated a `webhdfs://` file. Listings now
  set `etag` to the `GetVersionTag` value (`<mtime>:<length>`). Test: a repeated `count(*)` over 12 files sends only
  the listing.
- **hannes: the LISTSTATUS_BATCH guard caught only a repeat of the last page.** A server alternating A, B, A, B
  looped forever (round-1 C4 was incomplete). The guard now rejects any page that ends at a name an earlier page
  ended at. It still assumes no ordering. Unit test.
- **mytherin: a walk error was not sticky.** `Next()` had popped the failed item, so a later call carried on without
  its subtree. The first error is now rethrown on every later call. Core expands up to 500 files at bind time, so no
  SQL test reaches the re-scan without a fixture of more than 500 files. The change is small and local.
- **tmonster, kryonix: `webhdfs_ls` on a directory sent GETFILESTATUS twice.** The status is now handed to the
  walk. Covered by the request-count test.
- **Input checks:**
  - `webhdfs_block_locations` rejects a negative `offset` or `length` (hawkfish, dtenwolde).
  - xattr hex values are decoded strictly, where `strtol` accepted `-1`, `+f` and ` f` (tishj, dtenwolde). Unit
    test.
  - `webhdfs_xattrs(names := [])` returns no rows instead of every attribute (dtenwolde).
  - `CREATE SECRET` checks that `REPLICATION` is between 1 and 32767 and `BLOCK_SIZE` is positive (hawkfish,
    szarnyasg).
  - `CREATE SECRET` rejects a `SCOPE` no `webhdfs://` or `swebhdfs://` URL can match (dtenwolde).
- **hawkfish: the epoch bound was symmetric.** DuckDB's smallest TIMESTAMP is 290309-12-22 BC
  (-9223372022400000 ms), not -MAX. Unit tests at the bound and the bound minus 1.
- **hannes: `getpwuid` is not reentrant and runs on many threads.** Now `getpwuid_r`.
- **pdet: an unconfigured logical cluster name failed with only a DNS error.** The error now names the fix
  (`HADOOP_CONF_DIR` or a secret with `NAMENODES`).
- **Test gaps:**
  - The "parallel scans of one file" test used a one-row-group file. A 15-row-group fixture now runs under 4 threads
    across read-ahead sizes (pdet).
  - New tests cover `last_modified` from a listing against GETFILESTATUS (hawkfish), the listing schema with
    `DESCRIBE` (tmonster), and the httpfs-not-loaded precondition of the loading test (carlopi).
- **Docs:**
  - README:
    - Kerberos tokens are kept per identity (szarnyasg).
    - When environment fallbacks apply (szarnyasg).
    - Which `CREATE SECRET` checks exist (szarnyasg).
    - `VALIDATE_REMOTE` never revalidates `webhdfs://` (dentiny).
    - What a failed overwrite with `USE_TMP_FILE false` leaves (taniabogatsch).
    - Re-create persistent secrets made in the old custom format (carlopi, maxxen, tishj).
    - No WebAssembly build, and the Windows autocomplete workaround (szarnyasg, carlopi).
  - PLAN: the retry and `overwrite` rules, and the walk order (taniabogatsch, kryonix).
  - UPDATING.md: the jobs that exist (szarnyasg).
  - The Wasm reason in the workflow comment (carlopi).

## Found while verifying (coordinator)
- **DuckDB's CSV reader opens files by name.** `CSVFileHandle::OpenFile` reaches `OpenFileExtended` without the
  listing's `extended_info`, so every CSV file in a glob costs a GETFILESTATUS per open (2 per file: sniff and
  scan). Parquet and `read_text` are not affected. This is a core behaviour: documented in the README, and listed
  under O7.

## Disagreements
- **Own secret resolver vs core's matcher.**
  - For: samansmink and maxxen want authority-only scopes normalised to `scheme://authority/` at `CREATE`, so that
    core's prefix match is exact. That would let the custom `LookupSecret`, the copied storage tie-break offsets and
    the `which_secret()` mismatch go, and it would also fix `http` secrets (O2).
  - Against: tishj wants the path part segment-exact too (`/data` must not match `/database`), which core's prefix
    match can't express.
  - Both sides agree the current split, whole authority but prefix path, is a half step. Open as O3.
- **Fewer requests vs simpler code.**
  - Speed: tmonster, dentiny and lnkuiper want identity and principal caching, fewer copies and more windows per
    handle.
  - Benchmark first: mytherin and kryonix want a benchmark before more caching.
  - Nothing was changed for speed alone this round.

## Open: needs a design decision
- **O1. Replays of DELETE and exclusive CREATE** (lnkuiper, taniabogatsch: 2, medium).
  - Core retries every request but POST. A DELETE replayed after a lost response answers `false`, so `RemoveFile`
    reports "No such file" for a file it deleted.
  - A replayed DataNode `PUT` of a `CREATE overwrite=false` gets FileAlreadyExists for a file this handle wrote.
  - The options: reconcile with a GETFILESTATUS as `MoveFile` does (accept length == sent), or send those legs with
    `retries = 0`. Both change error behaviour, and neither can be tested without fault injection.
- **O2. The `http` secret's headers and bearer token go to every DataNode** (samansmink, hannes, maxxen on the same
  secret's prefix scope: 3).
  - Redirects are followed by the extension (`follow_location = false`), so curl's cross-host credential stripping
    never runs.
  - Restricting credentials to the NameNode or gateway host would break a Knox setup whose `Location` is the gateway
    itself unless the comparison is exact. `HTTPFSParams::bearer_token` is not visible to this extension, which links
    no httpfs code.
  - Needs: a host rule, and either an httpfs API to clear credentials or a second `InitializeParameters` call for the
    DataNode URL. Also refuse https-to-http redirects. Documented in the README limitations.
- **O3. Secret scope semantics** (samansmink, maxxen, tishj). See Disagreements. The legacy CUSTOM deserializer has
  no test fixture (tishj, maxxen). `loadable_smoke.py` checks reload with `which_secret`, which proves nothing about
  authority matching (tishj).
- **O4. Destructor commits whose failure is swallowed** (taniabogatsch, carlopi: 2, medium).
  - EXPORT DATABASE drops `schema.sql` and `load.sql` handles without `Close()`. If the implicit commit fails, the
    export reports success with the file missing.
  - Options: log through `DUCKDB_LOG_WARNING` (needs a failure-injection test), or fix upstream with
    `handle->Close()` in `WriteStringStreamToFile`.
  - Mid-write failure tests (failed APPEND or DataNode leg, Abort cleanup) need a quota-based fixture
    (taniabogatsch).
- **O5. Per-open identity cost** (dentiny, tmonster, mytherin, taniabogatsch: 4).
  - Each `CreateClient` does about 3 secret lookups and, with Kerberos, a `gss_inquire_cred` ticket-cache read. That
    happens for every file open and every scalar row.
  - Options: cache the principal with a TTL, cache clients per authority per chunk, or look up the `http` secret
    once.
  - A TTL lets a `kinit` as another user reuse the old identity for its duration, so it is a trade-off to choose.
- **O6. Upstream fixes to propose to DuckDB:**
  - autocomplete `PathSeparator(search_dir)`, which would let R2-5's guard go;
  - autocomplete catching listing errors (dtenwolde: a Tab on an unreachable cluster runs the full retries, and an
    error invalidates an open transaction in the shell);
  - a way for an extension to mark `webhdfs://` as remote for `VALIDATE_REMOTE` (dentiny);
  - CSV opens keeping `extended_info` (coordinator).
- **O7. Shared state bounds** (dtenwolde).
  - Endpoints are never evicted, and each holds up to 64 MiB of small files. Their names feed every session's
    autocomplete and `webhdfs_namenodes()` rows.
  - Fix: a database-wide byte budget, or an LRU over endpoints. Related: up to 3 copies of a small file (lnkuiper,
    dentiny).
- **O8. Smaller deferred items:**
  - `LIMIT` and cancel during a long listing (kryonix);
  - a thread pool per row in a LATERAL `webhdfs_ls` (kryonix, deferred in round 1);
  - an uninterruptible RETRIABLE backoff sleep (dentiny);
  - unbounded JSON and error bodies (hannes);
  - `core-site.xml` read with `ifstream` even with external access off (hannes);
  - `GetStatsIfExists` override (maxxen);
  - an unbounded literal-directory shortcut (tmonster);
  - unused `WebHdfsWalk::Stats()` (tmonster);
  - `fileId` in the version tag (hawkfish);
  - non-numeric JSON strings read as the default (hawkfish; changing it risks rejecting gateway responses);
  - duplicate `typeQuota` keys (tishj);
  - a 401 hint (szarnyasg);
  - CI cost: the integration job runs on both push and pull_request, and its benchmark asserts nothing (samansmink);
  - delete the legacy `ListFiles` and `Glob` overrides (mytherin);
  - TSan job (mytherin, deferred in round 1);
  - stamp check in the smoke test (carlopi).
- **Declined:**
  - kryonix's B1 point is right: `MetaBindData::Equals` can't be reached for in-out functions, so its test passes
    without the fix. The fix stays as defensive code and the test still checks the result.
  - hawkfish's `accessTime <= 0` vs `== 0` is cosmetic, since HDFS never reports negative access times.

## Recommendation
Merge the consensus fixes. mytherin's and kryonix's blocking items are fixed. The concerns left open from the other
"request changes" verdicts are:
- O1 (taniabogatsch: the exclusive CREATE replay);
- O2 (hannes, samansmink: `http` credentials sent to DataNodes);
- O4 (taniabogatsch, carlopi: a failed destructor commit);
- O6 and O7 (dtenwolde: autocomplete errors, unbounded endpoints).

O2 is the one to take next. It is a credential leaving its scope, and settling O3 at the same time would fix the
`http` and `webhdfs` secrets together.

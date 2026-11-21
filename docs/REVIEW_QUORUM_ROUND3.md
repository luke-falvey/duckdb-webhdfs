<!-- Output of the DuckDB Review Quorum (.github/agents), round 3, run on 2026-11-21: 15 simulated reviewer lenses,
each run as an independent read-only subagent over claude/quorum-round-2 at 0a48b76 (PR #5, not merged yet).
Consensus = raised by 3+ lenses, or by 2 with the same trace and medium/high severity. Per-lens reviews were working
notes and are not kept. -->
# Quorum round 3: duckdb-webhdfs after round 2 (PR #5)

Rounds 1 ([REVIEW_QUORUM.md](REVIEW_QUORUM.md)) and 2 ([REVIEW_QUORUM_ROUND2.md](REVIEW_QUORUM_ROUND2.md)) are done.
Each lens was asked not to re-raise what those rounds fixed or left open unless it had a new trace or a clear answer.
They looked for what was missed, what round 2's fixes broke or left half done, and new problems. The lenses are
simulations of reviewers' priorities, not the people.

## Verdicts
- **Request changes (3):** hannes, pdet, carlopi.
- **Approve with nits (12):** mytherin, lnkuiper, tishj, taniabogatsch, hawkfish, samansmink, maxxen, tmonster,
  dtenwolde, szarnyasg, kryonix, dentiny.

Most findings are about round-2 fixes that were half done. pdet's and carlopi's "request changes" rest on R3-3. hannes's
rests on a redirect-target access issue (O9), which is a round-1 deferral that dropped off the open list. No lens found
a crash or data loss on the normal read or write path.

## Consensus items (implemented)

**R3-1. The small-file cache size had two writers** (dtenwolde, szarnyasg, samansmink, pdet, lnkuiper, hawkfish,
tmonster, carlopi, maxxen, mytherin, dentiny, taniabogatsch: 12; R2-1 half done)
- R2-1's `OnSet` hook resized every cache, but every `WebHdfsClient` constructor also applied the size it had loaded.
- Core calls `OnSet` before it stores the value (`physical_set.cpp:29`, then `:32`). So a client built on another
  thread during `SET webhdfs_small_file_cache_size = 0` put the old 64 MiB back, and the cache refilled until the next
  client was built. It also cost a lock and an eviction pass on every file open.
- `SET ... = NULL` skipped the hook, though `Load` reads NULL as the default (dtenwolde, tishj, dentiny).
- Now:
  - The file system holds the size. Extension load sets it, `OnSet` changes it (NULL means the default), and
    `GetEndpoint` gives a new endpoint the current size. Clients no longer touch it.
  - The settings are registered after `RegisterSubSystem`. The hook holds a reference to the file system, which
    dangled if registration threw (tishj, mytherin).
- Test: `integration/webhdfs_requests.test` (`SET ... = NULL` turns the cache back on). The race needs two threads
  inside one statement's window, so no SQL test reaches it.

**R3-2. `webhdfs_namenodes()` was still half per-session** (path scope: dtenwolde, szarnyasg, samansmink, pdet,
lnkuiper, hawkfish, tmonster, kryonix, dentiny: 9; stale rows: samansmink, maxxen, tishj, mytherin, taniabogatsch: 5;
R2-3 half done)
- R2-3 resolved `auth_mode`, `namenode_source` and `secret_name` for the caller, but for the endpoint key
  `scheme://authority`, which parses to `/`.
  - A secret scoped to `webhdfs://nn/data` never matches `/`. Its row showed `simple`, `url` and NULL for a cluster
    that every request reached through that secret.
- The rows still list every NameNode any configuration recorded (`RecordConfig` only adds), so the fresh columns were
  written onto NameNodes they don't describe.
  - The repo's own test showed it: after `DROP SECRET loopback_tmp`, `localhost:3` was labelled `loopback`, and
    `SELECT DISTINCT` hid that.
- Now:
  - The three columns are documented as "as this session would resolve them for the cluster root". So a secret
    scoped to a path below the root is never shown, even for the NameNode its requests went to: with no `NAMENODES`
    of its own, its row shows the root's configuration (szarnyasg, PR review).
  - They are filled only on rows whose NameNode is in that configuration. A dropped secret's NameNode, another
    session's, or a path-scoped secret's `NAMENODES` get NULL.
  - `TryResolveConfig` runs the same `disabled_filesystems` and external-access gate as `CreateClient` (hannes,
    mytherin).
  - It no longer swallows every exception (dtenwolde, maxxen). An invalid secret (`InvalidConfigurationException`,
    `InvalidInputException`, or `ConversionException` for a mistyped value) shows as NULL; anything else is an error.
- Tests:
  - `webhdfs_secret_persistent.test`: row by row, no DISTINCT.
  - New `webhdfs_namenodes_session.test`: a root-scoped secret, a path-scoped one with and without its own
    `NAMENODES`, and the root refused by `allowed_directories` with external access off.
- Considered and not done: one row per matching secret, or a `secret_names` list. Both change the schema. They can come
  with O3.

**R3-3. The R2-7 access check refused allowed directories** (pdet, carlopi, maxxen, tishj, mytherin, hannes, dentiny,
taniabogatsch: 8; R2-7 copied an older problem into a second place)
- `CreateClient` checked every path as `FILE_TYPE_REGULAR`, after URL parsing had dropped the trailing `/`.
- Core checks directory operations as `FILE_TYPE_DIR`, which adds the `/`, and stores `allowed_directories` with one.
- So with `allowed_directories = ['webhdfs://nn/data/']`, core let `DirectoryExists`, `ListFiles` and `CreateDirectory`
  on `webhdfs://nn/data` through, and then the extension refused them. That broke `webhdfs_ls` and
  `webhdfs_content_summary` on the allowed directory, and listing or creating it.
- Two lenses also traced `COPY ... TO 'webhdfs://nn/data' (PARTITION_BY ...)`. That one is refused by core itself, which
  checks a COPY target as a file, so it is not a webhdfs bug.
- HTTPUtil's own check of `access_path` had the same problem before round 2, and it can't be fixed by adding a `/`:
  `SanitizeAllowedPath` canonicalises the path through this file system, which drops the slash again.
- Now:
  - `CreateClient` takes the access type. The directory overrides (`DirectoryExists`, `CreateDirectory*`,
    `RemoveDirectory*`, `ListFilesExtended`) and the metadata functions, which take a file or a directory, check as
    `FILE_TYPE_DIR`.
  - Those clients set `skip_external_access_check`, because `CreateClient` has just made the equivalent check as a
    directory.
  - File opens still check as regular files.
- Test: `integration/webhdfs_read.test`:
  - `webhdfs_ls` on the allowed directory, with and without the slash;
  - `webhdfs_content_summary` and `webhdfs_exists` on it;
  - `EXPORT DATABASE` to it, which reaches the file system's `DirectoryExists` and `CreateDirectory` on the directory
    itself. A COPY can't: core checks its target as a file, even with a trailing `/`;
  - a partitioned `COPY` below it (a regression guard: that passed before too);
  - a file in `allowed_paths` still accessible to the metadata functions;
  - its parent and a sibling with the same prefix (`/test/data2`) still refused.

**R3-4. `CREATE SECRET` accepted scopes that are a prefix of the scheme** (dtenwolde, szarnyasg, samansmink, pdet,
maxxen, tishj, hannes: 7; R2 input check incomplete)
- `ValidateScope` also accepted any scope the scheme starts with (`StartsWith(scheme, lower)`).
  - `'webhdfs:/'` became a catch-all for every `webhdfs://` URL.
  - `'WEBHDFS'` and `'w'` matched nothing, because the no-`://` branch of the matcher is case-sensitive. That is the
    "silently unused secret" the check was added for.
- Now the whole `webhdfs://` or `swebhdfs://` prefix is required, compared case-insensitively.
- Tests: `webhdfs_no_cluster.test` (`'webhdfs:/'`, `'WEBHDFS'`, `'w'`).

**R3-5. The listing `etag` made a weak tag the only validator** (hawkfish, tmonster, kryonix, lnkuiper, dentiny,
taniabogatsch: 6; the etag test: pdet, lnkuiper, kryonix: 3; R2 unique flag)
- With a non-empty version tag, `ExternalFileCache::IsValid` compares tags only and skips its 10-second
  last-modified guard.
- So R2's etag (`<mtime>:<length>`) decided alone whether Parquet metadata (and the small-file cache) was reused.
  HDFS lets clients set the mtime (`setTimes`, `distcp -pt`, `hdfs dfs -cp -p`). A same-length replacement with its old
  mtime kept would serve the old footer.
- The R2 test passed without the etag: the tiny files sat in the small-file cache, so a revalidation would also have
  sent no OPEN (pdet).
- Now:
  - The tag is `<fileId>:<mtime>:<length>`. An overwrite or rename onto the path makes a new inode. Both LISTSTATUS and
    GETFILESTATUS return `fileId`, so this costs no request.
  - Listed files carry `file_id` in `extended_info`, so opens from a listing still skip GETFILESTATUS.
  - A listed entry with a negative length gets no `etag` (dentiny).
- Tests:
  - `test/cpp` (`version_tag`): two statuses that differ only in `fileId` get different tags, a listing's etag is the
    handle's tag, a missing `fileId` gives -1, and a negative length gets no etag. The tag is built by
    `webhdfs::VersionTag` / `ListingEtag`, so that the unit tests reach it.
  - `integration/webhdfs_requests.test`: the etag section runs with the small-file cache off and pins every request:
    the warm-up scan, the repeat (only the listing), the scan without the metadata cache, and a Parquet file replaced
    by one of the same row count (read again with its new values).
  - The same-length, preserved-mtime case needs `setTimes`, which no SQL path can call; the unit test pins the tag.
- The guarantee needs a server that reports `fileId`. HDFS NameNodes do; without it the tag falls back to the mtime
  and length (README).
- tishj's related point is left open (O10): the Parquet metadata cache is database-wide and keyed by path, so one
  identity's cached row counts and statistics can serve another that may list the directory but not read the file.

**R3-6. `webhdfs_xattrs(names := [])` succeeded on a missing path** (dtenwolde, tishj, taniabogatsch: 3; R2 input check)
- R2's early return for an empty filter ran before any request, so a missing path gave 0 rows where every other
  filter raises FileNotFound.
- Now the empty filter sends a GETFILESTATUS first. `names := [NULL]` still selects nothing, as before.
- Test: `integration/webhdfs_functions.test`.

**R3-7. Docs that claim more than the code does** (szarnyasg, kryonix, mytherin, hannes, carlopi)
- Walk order (kryonix, mytherin):
  - Byte order holds for the files a glob expands to.
  - It does not hold for `webhdfs_ls`, which emits a matching directory after its subtree.
  - README, PLAN and the code comments now say so, and that `webhdfs_ls` needs `ORDER BY`.
- README Limitations (szarnyasg) now state these consequences instead of pointing at a review report:
  - O1: a retried DELETE or exclusive CREATE reports a false error.
  - O4: a failed implicit commit from `EXPORT DATABASE` is silent.
  - O2: `http` secret credentials reach DataNodes. The workaround is a `webhdfs` secret.
  - O9: redirect targets are not checked against `allowed_directories`.
  - A failed final `Close()` deletes the file, which DuckDB treats as an unknown outcome (carlopi).
- `SET redact_http_logs = false` exposes delegation tokens in `duckdb_logs`. PLAN wrongly said the log always has full
  URLs (hannes).
- The function table now says every table function starts with a `path` (or `endpoint`) column. The
  `webhdfs_namenodes()` row lists all its columns (szarnyasg).

## Unique flags adopted (cheap and clearly right)
- **hawkfish: the upper epoch bound was 1 ms short.** DuckDB's largest TIMESTAMP is `INT64_MAX - 1` µs, so the last
  whole millisecond is `(INT64_MAX - 1) / 1000`. Unit test at the bound and the bound plus 1.
- **mytherin: `webhdfs_ls` handed the walk a status for another path.** For a path with glob escapes (`a\*b`), the stat
  was of the raw path but the walk's root is the unescaped one. Now the walk gets the path glob-escapes
  (`webhdfs::EscapeGlob`), so its root is exactly the path that was stat'ed and checked, and it always gets that
  status. A path with no unescaped glob characters names itself, backslashes included, in every function (the user's
  decision A in the PR review loop, where the history is). Tests:
  - `test/cpp` `glob_escape_literal`;
  - `integration/webhdfs_functions.test`: directories `d\x` (with a decoy `dx`) and `\.\.`, a file `a\b.csv` from
    `webhdfs_ls`, `read_csv` and `glob()`, `a*b.csv` reached by patterns, `a\*b.csv` not found;
  - `webhdfs_read.test`: an escaped `..` stays inside the allowed directory.
- **lnkuiper, tmonster: the cached CSV read counted only OPEN.** The full request list is now pinned (2
  GETFILESTATUS, from core's CSV reader opening by name), so the upstream fix in O6 shows up as a test diff.
- **carlopi: `webhdfs_httpfs_loading.test` assumed autoloading was off.** It now has `require no_extension_autoloading`.
- **pdet: the access tests required httpfs without needing it.** `webhdfs_access_control.test` and
  `webhdfs_disabled_filesystem.test` no longer do, so they also run in builds without httpfs, and they show that the
  permission check comes before anything that needs httpfs.
- **kryonix: the order regression test used `list()` without ORDER BY.** It now compares rows in order.

## Disagreements
- **O3, the secret resolver** (unchanged from round 2, with more votes).
  - Normalise authority-only scopes at CREATE, let core's matcher pick the secret, and delete the custom resolver:
    samansmink, maxxen, dtenwolde.
  - Normalise, but keep the extension's own resolver with segment-exact paths: tishj.
  - R3-2 shows the cost of a custom scorer: every consumer must repeat the same path rule.
- **O1, how to handle a lost response.**
  - Send the DataNode leg of an exclusive CREATE with `retries = 0` and fail loudly: hannes, carlopi, tishj, lnkuiper,
    taniabogatsch.
  - For DELETE, taniabogatsch would treat `false` after a retried attempt as success. The others would send it with
    `retries = 0` as well.
- **O4, the implicit commit.**
  - Fix upstream only (`handle->Close()` in `WriteStringStreamToFile`): mytherin.
  - Also log the swallowed failure with `DUCKDB_LOG_WARNING` until then: samansmink, carlopi, taniabogatsch.
- **O5, per-open identity cost.**
  - Benchmark first, with injected latency: tmonster, mytherin.
  - Cache a client per chunk: lnkuiper.
  - Cache the OS user for the process: mytherin.
  - A principal TTL behind a setting: dentiny.
  - No TTL: hannes.

## Open: needs a design decision
O1 to O8 keep their round-2 numbers.
- **O1. Replays of DELETE and exclusive CREATE.**
  - The panel now leans to `retries = 0` on the exclusive-CREATE DataNode PUT (see Disagreements).
  - It stays open because no test can show it without a fault-injecting proxy, and every behaviour change needs a
    test.
  - The README now states the current behaviour.
- **O2 + O9. Credentials and access checks on redirect targets.** O9 is new from hannes (high), and is the round-1 C3
  deferral.
  - Every leg of a client shares one `HTTPParams`, whose `access_path` is the `webhdfs://` URL. So with
    `allowed_directories`, a NameNode (or, on plain HTTP, a man in the middle) can redirect a read or write to any
    host, such as `169.254.169.254`, and core's check passes. With `spnego_every_request`, Kerberos also asks the KDC
    for a ticket for that host.
  - samansmink, maxxen and hannes converge on one fix:
    - Build each DataNode leg's `HTTPParams` with `InitializeParameters` for the redirect URL. Core's secret matcher
      then decides which `http` credentials go there, which fixes O2.
    - Refuse https-to-http redirects.
  - Still to decide: what an allowed DataNode looks like when access is restricted. Options are an explicit
    `allowed_directories` entry per DataNode, only NameNode hosts and `DATANODE_ADDRESS_MAP` targets, or a secret
    option. Each choice breaks some existing setup.
  - Documented in the README limitations now.
- **O3. Secret scope semantics.** See Disagreements. The legacy CUSTOM deserializer still has no fixture (tishj,
  maxxen).
- **O4. Destructor commits whose failure is swallowed.**
  - New evidence (carlopi): when a final `Close()` fails, core moves the handle aside as "unknown" and does not abort
    it. The webhdfs destructor then deletes the file, though the last APPEND may have been applied. Only
    `AbortFileWrite` should delete.
  - taniabogatsch traced that every COPY writer closes or aborts through `AsyncFileWriter`, so the implicit commit only
    serves `EXPORT DATABASE` and log storage.
  - Changing it needs a failure-injection fixture. Documented in the README now.
- **O5. Per-open identity cost.** See Disagreements. Add: compute `CurrentOsUser()` once per process (it runs on every
  open with simple auth and no user name), and retry `getpwuid_r` on `ERANGE` (mytherin).
- **O6. Upstream fixes.** As in round 2, plus:
  - an `OnSet` that runs after the value is stored, or a post-set hook (maxxen; R3-1 works around it);
  - a typed access-check helper that extensions bypassing the VFS can call (carlopi; R3-3).
- **O7. Shared state bounds.**
  - Plus dtenwolde: list cluster names only from secrets and `dfs.nameservices`, not from endpoints, so endpoint
    eviction doesn't change what autocomplete offers.
  - Plus lnkuiper: the small-file cache's three copies (window, cache, out) could share one buffer.
- **O8. Smaller deferred items:**
  - closed: `fileId` in the version tag (R3-5);
  - declined: the `GetStatsIfExists` override (maxxen, dentiny: the default costs the same one GETFILESTATUS, or
    none from a listing);
  - still open from round 2:
    - LIMIT and cancel during a long listing (kryonix: check `IsInterrupted` in the page loop);
    - thread pool per LATERAL row (kryonix: add a LATERAL test first);
    - uninterruptible backoff (dentiny: `ThreadUtil::SleepMs` with the context);
    - unbounded JSON and error bodies (hannes: cap pages, read error bodies only up to the display limit);
    - `core-site.xml` read with `ifstream` when access is off (hannes);
    - unbounded literal-directory shortcut;
    - unused `WebHdfsWalk::Stats()`;
    - non-numeric JSON strings read as the default (hawkfish: reject a present but non-numeric `modificationTime`,
      now that it feeds the tag);
    - duplicate `typeQuota` keys (tishj: throw);
    - a 401 hint (szarnyasg, pdet);
    - CI cost (samansmink, pdet, tmonster: run integration on `pull_request` and pushes to `main` only, the benchmark
      by hand, `paths-ignore` for docs);
    - delete the legacy `ListFiles(string, bool)` override (mytherin);
    - TSan job;
    - stamp check in the smoke test (carlopi);
  - new:
    - the `delegation=` substring test in `DataNodeCall` should parse the query (hannes, low);
    - the `extension-ci-tools` submodule (`b8d325c`) is older than CI's `v2.0-cyanoptera` (carlopi). Bump it with the
      next DuckDB update, and say in UPDATING.md that the two must match;
    - the Wasm reason in the workflow comment should name per-variant blockers (carlopi);
    - request counts under `threads > 1` and with `union_by_name`/`hive_partitioning`/`filename` (lnkuiper, tmonster);
    - a bench with injected latency (tmonster);
    - malformed xattr hex reads as NULL silently (taniabogatsch);
    - tests for the `NULL_IF_EXISTS` early close and the sticky walk error, which need a C++ test against the cluster
      (taniabogatsch);
    - `webhdfs_block_locations` with `offset > 0` and the default length sends `offset + INT64_MAX`, which hawkfish
      recalls overflowing on the NameNode (one row instead of all). Clamping the length is harmless, but the HDFS
      behaviour is unverified here and needs a multi-block fixture;
    - an allowed directory set before `LOAD webhdfs` is not canonicalised (lowercase authority), so it can miss
      (tishj, carlopi).
- **O10 (new). Cross-identity Parquet metadata** (tishj, medium).
  - Core's Parquet metadata cache is keyed by path, database-wide. Since the listing etag, one identity's cached
    footer can answer another identity's `count(*)` without an OPEN, and HDFS checks read permission only on OPEN.
  - It is opt-in (`parquet_metadata_cache`), and S3 has the same design.
  - Options: document it, or mix the K1 identity key into the etag. The second would cost every identity its own
    cache entry.

## Recommendation
Merge R3-1 to R3-7. They complete round 2's half-done fixes and close pdet's and carlopi's blocking item (R3-3).

hannes's blocking item, O9, is now the top open item and should be fixed together with O2, using the per-leg
`HTTPParams` the panel agrees on. What remains is the policy decision for which DataNodes are allowed when external
access is restricted. Next in line are O1 (`retries = 0` for the exclusive-CREATE leg, once a fault-injection fixture
exists) and O3.

## PR review loop
After the PR was opened, the 15 lenses reviewed the PR diff itself (read-only, each with the diff and the code around
it) until none requested changes. At most 5 iterations; a finding raised again after a fix, or two lenses asking for
opposite things, would stop the loop and be listed here.

### Iteration 1 (on 462878f)
- **Verdicts:** request changes 4 (szarnyasg, carlopi, pdet, kryonix); approve with nits 11.
- **Request changes:**
  - szarnyasg: the README said a path-scoped secret makes the `webhdfs_namenodes()` columns NULL. That holds only
    when the secret has its own `NAMENODES`. One without them shows the root's configuration. Fixed: the README and
    R3-2 now say the columns describe the cluster root and are never a path-scoped secret's, with a test row for that
    case.
  - carlopi (also hawkfish, tmonster, lnkuiper, dtenwolde and tishj as nits): no test failed if `fileId` was dropped
    from the tag. Fixed: the tag moved to `webhdfs::VersionTag`/`ListingEtag` and has a unit test.
  - pdet, kryonix (also samansmink, mytherin, maxxen, hannes, tishj, dentiny and taniabogatsch as nits): no test
    reached the file system's directory overrides on the allowed directory itself; the partitioned COPY wrote below
    it. Fixed with `EXPORT DATABASE` to the allowed directory (kryonix's path). pdet's `COPY ... TO '.../allowed_dir/'`
    doesn't work: core checks a COPY target as a file and refuses it, slash or not.
- **Nits fixed:**
  - every request pinned in the etag section; the replaced file keeps its row count (tmonster, lnkuiper, dentiny,
    pdet, kryonix);
  - a duplicate `truncate_duckdb_logs()` (all 15);
  - `WebHdfsSettings::small_file_cache_size` is gone: a dedicated `LoadSmallFileCacheSize` runs once at load
    (samansmink, lnkuiper, mytherin, maxxen);
  - `CreateClient` passes the `FileType` to `CreateParams`, which alone decides to skip HTTPUtil's check, with the rule
    that params for another URL (O9's DataNode legs) must not take the skip (mytherin, hannes, maxxen, carlopi,
    samansmink);
  - `file_id` goes into `extended_info` only when the server sent one (hawkfish, maxxen);
  - README: the `fileId` guarantee needs a server that reports it; all `webhdfs_namenodes()` columns and every NULL
    case; the O9 limitation names the man in the middle and the KDC request; which calls treat an allowed directory as
    accessible (szarnyasg, carlopi, hawkfish, pdet, maxxen, dtenwolde, tishj, taniabogatsch, dentiny);
  - `TryResolveConfig` also maps `ConversionException` to NULL, and its comment names the caught types and no longer
    claims a `HADOOP_CONF_DIR` error it can't reach (hannes, pdet, kryonix, dtenwolde);
  - `webhdfs_ls` stats the unescaped path, with a test (mytherin, dentiny, tishj);
  - tests: `webhdfs_exists` on the allowed directory, an `allowed_paths` file, the `/test/data2` sibling, the
    `xattrs(names := [])` request count, and `SCOPE 'webhdfs://'` accepted (tmonster, carlopi, hannes, kryonix,
    dtenwolde);
  - comments: the scope rule's rationale, and why extension load can seed the cache size (dtenwolde, lnkuiper).
- **Declined:**
  - samansmink: log a warning when a secret is invalid in `TryResolveConfig`. The NULL is documented, and the function
    has no logging context of its own.
  - maxxen: gate `webhdfs_namenodes()` only on `enable_external_access`, not the path check. Requests use the path
    check, so the function follows it; the README documents the NULL.
  - mytherin: a TSan run of a concurrent `SET` against scans. It stays with the TSan job in O8.
- **Pre-existing, noted for later:**
  - carlopi: a persistent secret saved before this PR with `SCOPE 'webhdfs:/'` still loads (deserialization does not
    validate, so other secrets keep loading). Re-create it.
  - hannes: `ListFilesExtended("webhdfs:/")` (autocomplete's cluster names) reads secrets and `SiteConfig()` without
    `CheckAccess` when called outside the VFS.
  - maxxen: `MoveFile` checks only the source itself; core's VFS checks both, and the target belongs with O9.
  - lnkuiper: the small-file copies (O7).

### Iteration 2 (on 6ce6d3b)
- **Verdicts:** request changes 9 (hannes, pdet, hawkfish, kryonix, tishj, dtenwolde, taniabogatsch, samansmink,
  maxxen); approve with nits 6 (szarnyasg, mytherin, carlopi, dentiny, lnkuiper, tmonster).
- **Request changes, all on one iteration-1 change.** Iteration 1 made `webhdfs_ls` stat the unescaped form of a path
  with no glob characters. Two problems:
  - Access bypass (hannes, pdet, kryonix, tishj, dtenwolde, samansmink, maxxen). The access check saw
    `.../data/\.\./x`, but the request went out for `.../data/../x`. A server that folds `..` would answer for a
    path outside `allowed_directories`, and the glob compiler's guard against exactly this never ran.
  - Semantics (hawkfish, carlopi, taniabogatsch, lnkuiper, tmonster). `read_*`, `glob()` and every other function
    still take such a path literally, so one URL named two files depending on the function. A real backslash in a
    name could no longer be listed.
  - Done: reverted to the raw-path stat, with the status handed to the walk only when the paths are equal. Tests: an
    escaped `..` doesn't reach outside the allowed directory or a parent; `a\*b.csv` is not found; patterns
    (`a*b.csv`, `a[*]b.csv`, `a\*b*`) find `a*b.csv`. The README states the rule.
- **Nits fixed:**
  - extension load seeds the cache size only if no `SET` reached the hook first (mytherin, dentiny, samansmink,
    maxxen);
  - `CreateClient` takes the access type with no default (mytherin);
  - `webhdfs_checksum` and `webhdfs_block_locations` check as a file, with a checksum test under the access settings
    (tmonster, pdet);
  - a `D_ASSERT` in `DataNodeCall` (taniabogatsch, hannes, maxxen). As written it could never fail; iteration 3
    replaced it;
  - a negative `fileId` reads as -1 everywhere (lnkuiper);
  - `WebHdfsSettings::Load` outside the narrowed catch (carlopi);
  - the scope error names an example;
  - README:
    - the SCOPE case rule;
    - the SPNEGO wording, without the internal flag name;
    - the `fileId` sentence split, with the in-place change that keeps its tag;
    - which functions ask about the cluster;
    - the escape rule (szarnyasg, dentiny, samansmink, lnkuiper, hawkfish);
  - tests:
    - RESET turns the cache back on (taniabogatsch);
    - an allowed cluster root still resolves in `webhdfs_namenodes()` with access off (tmonster);
    - comments on the parent test, the footer-only OPEN count and the O6 CSV count (szarnyasg, tmonster, dentiny).
- **Declined:**
  - hawkfish: a unit test that the epoch bounds give finite timestamps. The unit tests build without DuckDB, and the
    parser bound they pin is the one that matters.
  - maxxen, mytherin: a named "already checked" input instead of the `FileType`. The required `FileType` keeps the
    choice explicit at every call site; iteration 3 made the `D_ASSERT` say what holds.
  - carlopi, taniabogatsch: a test of the `ConversionException` catch. No SQL path reaches it (`CREATE SECRET` casts
    first); it stays as defensive code for the legacy secret format.
  - pdet: tests for `\\` and a trailing `\`. They matter only if paths are unescaped, which is the open dispute.

### Paused: two lenses wanted opposite things
The loop paused here under its safeguard. Iteration 1 asked for one thing and iteration 2 for its opposite:
- mytherin, dentiny and tishj (iteration 1): a path with no glob characters left should be unescaped (`a\*b` names
  `a*b`), as the walk treats it.
- hawkfish, carlopi, taniabogatsch and pdet (iteration 2): it must name the same file in every function. tmonster and
  lnkuiper would get there by unescaping in `read_*`, `glob()` and every metadata function as well.

The PR keeps the behaviour before round 3: such a path names itself. That is consistent across functions and can't
traverse. (Iteration 3 corrected two claims made here. DuckDB's local files do treat `a\*b` as a pattern, so this is
not "as in DuckDB". And `webhdfs_ls` on a directory did not yet follow the rule.) To decide:
- **A. Keep it** (the PR as it stands). The README tells users to reach `a*b` with a pattern such as `a[*]b`.
- **B. Unescape everywhere.** Every entry point unescapes, behind the glob compiler's `.`/`..` guard, and the access
  check runs on the unescaped path. It changes what `read_csv('.../a\b.csv')` opens for names with real backslashes.

**Decided by the user: A.** A path with no glob characters names itself. A finding that only re-argues
A against B is declined with a pointer here. The loop then continued with iteration 3, on the iteration-2 fixes.

### Iteration 3 (on 9aea5f4 + ceefb75)
- **Verdicts:** request changes 15.
- **Request changes, all on one finding (all 15).** Under A, `webhdfs_ls` on a directory still did not name itself.
  It stat'ed `x\y` but handed the same string to the walk, whose compiler unescapes the root. So it listed `xy`: zero
  rows, another directory, or a path the access check never saw.
  - Fixed: the walk gets `EscapeGlob(path)`, whose root is exactly the path, and always the stat'ed status. That is
    one GETFILESTATUS, for the checked path.
  - Tests: a directory `d\x` beside a decoy `dx`; a directory named `\.\.`; a file `a\b.csv` reached by
    `webhdfs_ls`, `read_csv` and `glob()`; the escape round-trip and the `..` guard in `test/cpp`.
- **Also raised by several and fixed:**
  - The iteration-2 `D_ASSERT` compared the client's params with the URL they were built from, so it could never fail.
    Since checksum and block locations check as a file, no directory client reaches a DataNode, so `DataNodeCall` now
    asserts that the access check was not skipped. The iteration-2 record and decline reason are corrected (carlopi,
    dtenwolde, maxxen, samansmink, hawkfish, mytherin, tmonster, kryonix, dentiny, tishj, lnkuiper).
  - No test showed checksum and block locations refusing the allowed directory itself. `webhdfs_access_control.test`
    now does it without a cluster (carlopi, maxxen, tmonster, szarnyasg, tishj, kryonix).
  - `SeedSmallFileCacheSize` also resizes endpoints that already exist (carlopi, maxxen, tmonster, lnkuiper, hawkfish).
  - `MetaCall::Client` has no default access type either (maxxen, lnkuiper).
  - README: "as in DuckDB" was wrong, because DuckDB's local files treat `a\*b` as a pattern, and "no glob characters
    left" read backwards (szarnyasg).
  - The escaped-`..` access test now aims outside both allow lists (szarnyasg, kryonix).
- **Smaller fixes:**
  - `fileId` 0 (HDFS's unknown id) reads as -1 too (taniabogatsch);
  - comments on the hook and the narrowed catch (taniabogatsch, hawkfish).
- **Declined:**
  - tmonster: a separate request-count pin for a backslash directory. The pinned `webhdfs_ls` of a plain directory
    (one GETFILESTATUS) goes through the same code.
  - Nothing re-argued A against B.


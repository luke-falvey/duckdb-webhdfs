<!-- Output of the DuckDB Review Quorum (.github/agents) run on 2026-11-20: 15 simulated reviewer lenses, consensus = raised by 3+ lenses. -->
# Quorum consensus: duckdb-webhdfs repo-wide review

## Verdicts
- **Request changes (13):** mytherin, hannes, lnkuiper, carlopi, tishj, taniabogatsch, hawkfish, samansmink, maxxen,
  tmonster, dtenwolde, kryonix, dentiny.
- **Approve with nits (2):** szarnyasg, pdet.

Per-lens reviews were working notes and are not kept in the repo.

## Consensus items (implemented)
These were raised by 3 or more reviewers, or are a high-severity unique flag that is cheap and clearly correct.

**K1. Identity-scoped cluster state** (mytherin, samansmink, dentiny, dtenwolde, taniabogatsch, lnkuiper, tishj: 7)
- The Kerberos delegation token, `spnego_every_request`, the DataNode redirect cache and the small-file cache are
  keyed by authority only.
- Key them by identity as well: auth mode + user_name + proxy_user + Kerberos principal (+ delegation token if given).
- A token fetched for doas=alice must never serve bob.
- DROP or REPLACE SECRET must not leave a usable token for a different identity. Keying by identity covers this:
  a new secret means a new identity key only if its fields change. That is acceptable.

**S1. Persistent secret serialization** (samansmink, maxxen, carlopi, tishj: 4)
- Serialize as KEY_VALUE_SECRET, so that sessions without webhdfs loaded can still read every persistent secret.
- Keep the exact-authority scope semantics. The extension must not rely on `WebHdfsSecret::MatchScore` after a
  round-trip. When resolving a secret, score candidates itself with `WebHdfsSecret::ScopeMatchScore` over all
  webhdfs secrets.
- Test: with a persistent secret saved, a fresh DB without webhdfs loaded can run `FROM duckdb_secrets()`.

**S2. CREATE SECRET validation** (pdet, dtenwolde, tishj, szarnyasg partly: 3+)
- AUTH 'token' requires DELEGATION_TOKEN.
- DELEGATION_TOKEN with AUTH 'kerberos' or 'simple' is an error.
- Each NAMENODES entry must be host:port, with no scheme and no path.
- Remove the unreachable unknown-option branch, or keep it as defensive code.
- Tests must give expected error text: no empty `statement error` expectations (tishj, lnkuiper, dtenwolde).

**W1. Write lifecycle** (carlopi, taniabogatsch, tishj, dentiny: 4)
- No network I/O from the destructor. An unclosed writer is abandoned: best-effort delete of what it created,
  never a commit.
- A failed APPEND must not re-send a chunk that the server may already have applied. Mark the handle failed so
  that later writes and Close throw.
- Set `closed` and `created` correctly so that Abort can clean up.
- ExclusiveCreate and ReturnNullIfExists must CREATE with overwrite=false.

**W2. Atomic MoveFile** (dentiny, taniabogatsch, lnkuiper: 3)
- Use RENAME with `renameoptions=OVERWRITE` (rename2) when the target may exist.
- Treat a replayed RENAME whose source is gone and whose target exists as success.

**R1. Read-ahead lock held across network I/O** (dentiny, pdet, mytherin, lnkuiper: 4)
- Fetch outside the lock, then install the window under the lock.

**C1. Small-file cache invisible / external file cache claim false** (tmonster, dentiny, samansmink, mytherin,
lnkuiper: 5)
- Core's ExternalFileCache treats `webhdfs://` as local, so it caches only with `SET cache_local_files = true`.
- Fix the README.
- Add a setting `webhdfs_small_file_cache_size` (default 64 MiB; 0 disables it).
- Clear the cache when identity keys change. K1 handles this.

**E1. Error classification by message text** (mytherin, lnkuiper, dtenwolde, tmonster: 4)
- The walk uses `ListStatus(missing_ok)` instead of matching message text for "not found".
- Unreachable or other errors are not retried with a fixed sleep in the walk. Core and the client already retry
  transport errors.

**J1. Checked integers from server JSON** (hawkfish, hannes, tishj: 3)
- Parse with range checks. Out-of-range values or negative lengths raise an IOException with context, not an
  InternalException or undefined behaviour.

**H1. httpfs detection by provider-name prefix** (samansmink, maxxen, carlopi: 3)
- Also accept `db.ExtensionIsLoaded("httpfs")`.
- Don't retry install before every request when autoload is off.
- Keep the error cause from the failed load.

**B1. `MetaBindData::Equals` ignores named parameters** (dtenwolde, taniabogatsch, kryonix: 3)

**D1. Stale docs** (szarnyasg, maxxen, samansmink, hannes, taniabogatsch, mytherin, kryonix: 7)
- PLAN.md: partition pruning, RedactUrl/logging, the Allocator buffer, token renewal, the removed files.
- README: the secret option table, AUTH precedence, failed-write behaviour, the timestamp column types, and the
  external file cache.

## Unique high-severity flags adopted
- **hannes C1:** resolve `.` and `..` in URL paths, so that `allowed_directories` can't be bypassed. Reject `..`
  that would go above the root.
- **hannes C4:** bound the LISTSTATUS_BATCH loop. Stop on an empty page or no progress.
- **hawkfish C1:** `accessTime` 0 becomes NULL `last_accessed`.
- **hannes C8 / tishj T7:** RedactUrl is dead code. Use it where URLs appear in error messages, or delete it.

## Deferred: needs a design decision, not consensus
- Own retry loops vs core `HTTPRetryBudget` (mytherin, lnkuiper). This is a larger refactor.
- 64 MiB write buffers outside the memory limit (dentiny, lnkuiper: 2).
- TSAN/debug CI job (mytherin, taniabogatsch: 2).
- Wider test matrices: option combinations, buffer sizes, LATERAL.
- Glob override and file ordering (maxxen, mytherin, kryonix).
- `Copy()` re-walk (tmonster).
- Redirect host and scheme policy (hannes C3).
- Thread-pool walk outside the scheduler (hawkfish).
- Optional CI platforms (carlopi).

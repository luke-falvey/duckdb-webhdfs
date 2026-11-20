# DuckDB reviewer personas

VS Code custom agents (`*.agent.md`). Each one is the review lens of a main DuckDB contributor or
maintainer. Use them together to get several independent opinions on this extension.

Each lens is built from public activity in `duckdb/duckdb` between Nov 2023 and Nov 2026:

- every PR the person authored (titles and descriptions), from GitHub search;
- the PRs by others they reviewed or commented on;
- the code areas they changed most, from `git log`;
- the comment threads of about 15 of their most-discussed PRs, read from the public PR pages.

Each file lists its evidence under "Evidence base".

They are simulations of priorities, not the people. Never present their output as what a real
contributor thinks.

## Usage

- **One lens:** pick an agent such as "Mark Raasveldt (DuckDB reviewer persona)" in the Chat
  agent picker, and ask it to review a file, diff or design.
- **Quorum:** pick **DuckDB Review Quorum**. It runs every persona as an independent subagent, then
  reports a verdict tally, consensus, disagreements, single-reviewer flags and a recommendation.

## Roster

Chosen by PRs authored, reviewed and commented on since Nov 2023.

| Agent | Who | Main lens |
|---|---|---|
| duckdb-mytherin | Mark Raasveldt | Extensible internal APIs, contracts set by types, thread safety, one change per PR |
| duckdb-hannes | Hannes Mühleisen | Security boundaries, crash-free handling of hostile input, few dependencies, sensible defaults |
| duckdb-lnkuiper | Laurens Kuiper | One owner per lifetime and outcome, zero-copy and memory-aware I/O, exact-behaviour tests |
| duckdb-carlopi | Carlo Piovesan | Builds on every platform, ABI and serialization compatibility, I/O failure paths, install and load UX |
| duckdb-tishj | Tishj | Edge-case correctness, tests that run in every config, ownership across API boundaries |
| duckdb-taniabogatsch | Tania Bogatsch | Durability across restarts, strict C API contracts, phased deprecation |
| duckdb-hawkfish | Richard Wesley | Time and time-zone correctness, PostgreSQL semantics, edge values |
| duckdb-samansmink | Sam Ansmink | Low CI and maintenance load, secret and setting semantics, extension ABI and versioning |
| duckdb-maxxen | Max Gabrielsson | Stable, versioned extension APIs; extending existing hooks; structured types |
| duckdb-pdet | Pedro Holanda | Tests on real file shapes, opt-in changes for existing users, bind-time errors |
| duckdb-tmonster | Tmonster | Strict benchmarks, tests that combine options, fewer remote requests and HTTP redirects |
| duckdb-dtenwolde | Dtenwolde | User control over extension behaviour, edge-case semantics, bounded shared caches |
| duckdb-szarnyasg | Gabor Szarnyas | Docs for every user-visible change, accurate claims, error messages that link to docs |
| duckdb-kryonix | Denis Hirn | Rewrites that keep semantics, measured performance at scale, narrow extension hooks |
| duckdb-dentiny | dentiny | Fewer remote calls and syscalls, HTTP caching per RFC, FileSystem semantics across platforms |

## Limits

- GitHub's search API does not return comment text. Review comments come from public PR pages,
  where inline review threads are often collapsed. Each lens therefore rests more on top-level
  comments, review summaries and authored PRs than on line-level review comments.
- A login is used as the name where git history has no full name.
- The `tools` names in the frontmatter (`read`, `search`, `web`, `agent`) follow VS Code's built-in
  tool sets. If your VS Code version uses other names, edit the frontmatter.

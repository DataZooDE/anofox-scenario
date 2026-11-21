# Updating DuckDB

anofox-scenario builds against the internal C++ API of DuckDB, which is not stable between
releases. One source tree serves two DuckDB lines at the same time:

| Line | DuckDB | extension-ci-tools | Where |
|------|--------|--------------------|-------|
| 2.0 (cyanoptera) | `v2.0.1` (`8d78114e90a53823803660bf821f6073ac87da8c`), the `./duckdb` submodule pin | `v2.0-cyanoptera` (also the `./extension-ci-tools` pin) | local builds, `duckdb-v2_0_1-*` CI jobs, `Next20Tests.yml` (macOS from source), DuckLake integration job, community-extensions `repo.ref` |
| v1.5 (variegata) | `v1.5.6` | `v1.5-variegata` | `duckdb-v1_5_5-*` CI jobs, community-extensions `repo.variegata` |

duckdb/community-extensions builds `repo.ref` against its `DUCKDB_LATEST_STABLE` (2.0 line) and
`repo.variegata` against v1.5.6, so both have to build from the same commit. Both lines are
smoke-tested and deployed by `MainDistributionPipeline.yml`, each under its own DuckDB version path.

## The compat-shim approach

Call sites never test the DuckDB version. Every API difference goes behind a helper that detects
the API by its *shape*: two overloads, one that only compiles against the new API (SFINAE on the
trailing return type), one for the old API; the caller passes `0` so the new one wins wherever it
exists. A version number cannot express the truth - v1.5.6 already carries some 2.0 APIs as
backports (e.g. `Identifier`, `CreateInfo::SetSchema`).

- **Generic helpers** live in [DataZooDE/duckdb-compat](https://github.com/DataZooDE/duckdb-compat),
  a header-only submodule (`./duckdb-compat`, namespace alias `dc`). It is wired as an include
  path only (`include_directories(duckdb-compat/include)`): never `target_link_libraries` it,
  DuckDB exports the extension targets and the generate step would fail. A helper any extension
  could need goes there, with a case in its `test/probe.cpp` compiled against every DuckDB line.
- **Scenario-specific shims** for storage internals (`DataTable`, index entries / ART,
  unique masks, table filters, `PhysicalMergeInto`, ...) live in
  `src/include/catalog/scenario_compat.hpp` (namespace `scenario_compat`), in the same style.

Rules of thumb for the 2.0 port (details in `docs/spec/architecture.md`, *DuckDB versions*):

- Catalog, schema, table and column names: `dc::Ident(string)` into DuckDB, `dc::Str(x)` out of
  it (2.0's `Identifier` converts explicitly both ways, and formats quoted in error messages -
  pass `dc::Str(...)` to exception arguments).
- 2.0 vectors carry their own size: size chunks with `SetChildCardinality(n)` (on 1.5.6 the same
  as `SetCardinality`), standalone vectors filled in place with `dc::SetVectorSize`, constant
  vectors with `dc::ReferenceValue(vector, value, count)`.
- `EntryLookupInfo` keeps a *reference* to its name on 1.5.6: never pass a temporary
  (`dc::EntryName` passes the caller's string through there).
- Behaviour, not just compilation, changed in places: 2.0 registers scenario DML targets as
  modified databases (demoted again in `ScenarioClientState::OnFinalizePrepare`), runs the
  actions of a `MERGE INTO` as concurrent pipelines (scenario sinks serialize on
  `ScenarioClientState::sink_lock`), and rejects a `MERGE` that matches one target row twice.

## Bumping a line

1. **2.0 patch release** (e.g. v2.0.1 -> v2.0.2): bump `./duckdb` to the tag and
   `./extension-ci-tools` to the head of `v2.0-cyanoptera`, `duckdb_version` (and the job names) of
   the `duckdb-v2_0_1-*` jobs in `.github/workflows/MainDistributionPipeline.yml`, and
   `DUCKDB_COMMIT` in `.github/workflows/Next20Tests.yml`.
2. **v1.5 patch release**: set `duckdb_version` in the `duckdb-v1_5_5-*` jobs. The v1.5 line is
   built locally in a scratch worktree whose `duckdb` is at that tag and `extension-ci-tools` at
   `v1.5-variegata`; the submodule pins stay on 2.0.
3. Survey every compile error against the exact commit (`ninja -k 0`), map each one to an
   existing helper or add a new one (generic: duckdb-compat PR with probe cases for every line;
   scenario internals: `scenario_compat.hpp`).
4. Run the suite through `build/<type>/test/unittest "test/*"` on both lines, deleting the
   `unittest` binary before each build so a failed build cannot leave a stale green binary.
   On 2.0 the extension is not linked into `unittest` or the shell: the sqllogictest runner loads
   the built file, `test/cpp` falls back to `LOAD '<build>/extension/anofox_scenario/...'`, and
   the shell needs `-unsigned` plus an explicit `LOAD '<path>'`.
5. DuckLake tests (`DUCKLAKE_TESTS=1`) need published ducklake binaries for the build's DuckDB
   version (core publishes them for tagged releases such as v2.0.1, not for pre-release commits).

Where a DuckDB release legitimately changes a result, keep the test and make it accept both
lines rather than dropping it. Current cases:

- `scenario_function_docs.test`: 2.0 renders parameter names from the signature (named
  parameters in declaration order); the named parameters are compared as a sorted set.
- `attach_perf.test`: 2.0 prints pushed filters as `id = 123456` (1.5: `id=123456`).
- `anofox_scenario_load.test`: checks `loaded` - on 2.0 the runner loads the built file, which is
  not "installed".
- `attach_double_touch.test`, `attach_keyless_merge.test`: a `MERGE` matching one target row twice
  fails with 2.0's own cardinality error (`... same target row more than once`) before the
  scenario's `same row twice` check; `WHEN MATCHED THEN DELETE` matched twice deletes the row once
  on 1.5 but fails on 2.0, on plain tables as on scenarios - the test runs it with
  `statement maybe` against a plain-table oracle and requires the same end state.

## Finding out what changed

- DuckDB's [release notes](https://github.com/duckdb/duckdb/releases) and the history of
  [core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the header whose API changed
- v1.5.6 marks its 2.0 backports with `NOTE(backport)` comments in the headers

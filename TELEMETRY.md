# anofox_scenario Telemetry

`anofox_scenario` collects **anonymous, privacy-preserving usage telemetry** so
we can see which capabilities are used, on which platforms, and where they fail
— and prioritise accordingly. It is **on by default** and **trivial to turn
off**.

Telemetry is emitted through the shared
[`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library and follows the cross-product **`telemetry_schema: 2`** envelope
(`posthog-telemetry/TELEMETRY-SCHEMA.md`). Ingestion is the EU PostHog cloud.

## How to turn it off

Any one of these fully short-circuits telemetry — when disabled, **nothing
leaves the machine** (the opt-out is enforced at the transport, not just at the
call sites):

```sql
SET anofox_scenario_telemetry_enabled = false;   -- DuckDB setting (per session)
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1                -- environment (1|true|yes)
```

Telemetry is also auto-disabled when a CI environment is detected (`CI`,
`GITHUB_ACTIONS`, `GITLAB_CI`, and similar).

## The guarantee: bounded, enumerated, non-PII

Every property we send is **either** a constant drawn from a small,
code-controlled enumeration **or** a pure number (durations, counts). The
library additionally clamps every outgoing string to 512 bytes as a backstop.

We **never** send: scenario names, schema names, table names, column names,
row/result data, cell values, protocol note content, `WHERE`/`FILTER` clauses,
SQL text, or error messages. Only the fixed strings and numbers described below
leave the machine.

The instrumentation is centralised in the extension entry point
(`src/anofox_scenario_extension.cpp`) and the shared telemetry library header
(`posthog-telemetry/include/telemetry.hpp`).

## What is collected

### Envelope (attached to every event)

`product` (`anofox_scenario`), `product_version`, `product_edition` (`oss`),
`telemetry_schema` (`2`), `duckdb_version`, `os`, `arch`, `platform`, `is_ci`,
`is_container`, a per-process `$session_id`, and — once associated — the
`deployment` group. `distinct_id` is the SHA-256 of a machine id: a **stable,
pseudonymous** identifier, not tied to any personal data.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `extension_loaded` | the `anofox_scenario` extension loads | — |

`extension_loaded` fires once, at extension load, from `LoadInternal`. This
repository currently emits only the `extension_loaded` envelope and adds no
per-function instrumentation.

## Function-call aggregation

The shared library also supports an aggregated `function_executed` event via
`RecordFunctionCall(function_name)`, which collapses in-process into a single
event per function per session (carrying `call_count` and `duration_ms_p50`),
flushed at session end. Instrumentation, when added, is placed at
bind/register time, never on a per-row `GetChunk` path, so a large scan produces
O(1) telemetry rows, not a firehose. `anofox_scenario` does not currently
instrument any function calls.

## Enterprise / account analytics

OSS `anofox_scenario` associates only the `deployment` group. It has no license
key, so no `account` group is associated.

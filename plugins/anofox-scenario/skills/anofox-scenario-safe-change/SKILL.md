---
name: anofox-scenario-safe-change
description: >
  Dry-run a data change in an anofox_scenario branch, review exactly what it would do,
  then apply it to the real tables or throw it away. Use when the user wants to test an
  ETL step, migration, bulk UPDATE/DELETE/MERGE, price change or data fix before applying
  it, asks for a "dry run" or "preview", wants to merge a scenario back into the base, or
  needs to resolve merge conflicts. Covers scenario_create + ATTACH, DML in the branch,
  validation queries, scenario_diff / scenario_diff_summary, scenario_merge_preview,
  scenario_merge with on_conflict := 'abort' | 'ours' | 'theirs', transactions around a
  merge, DuckLake merge-back, and discarding with scenario_drop.
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — Safe Change Playbook (dry-run, review, merge)

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0

Run the change against a branch, check the result, read the diff, preview the merge, then
apply it in one step — or drop the branch and nothing ever happened. For error messages
and edge cases see `anofox-scenario-reference`.

## Critical gotchas

- **Preview first.** `scenario_merge_preview(s)` lists every action the merge would take and
  flags conflicts, with no side effects. `scenario_merge` with the default
  `on_conflict := 'abort'` fails on the first conflict and changes nothing.
- **A merge is final for that scenario.** On success the scenario is frozen, `merged_at` is
  set and its delta is empty. A second merge fails (`already merged back into the base`).
  Drop it afterwards, or create a new scenario for the next change.
- **Merge refuses while branches exist** and while the scenario is frozen
  (`Unfreeze it before merging`). Drop branches first.
- **Foreign keys are checked at merge time, not inside the scenario.** A scenario may insert a
  child whose parent exists only in the scenario; the merge applies parents and children
  together and rolls back as a whole on a violation.
- **Host-database merges run in your transaction.** Wrap `scenario_merge` in
  `BEGIN ... COMMIT` to check the result before it becomes visible, or `ROLLBACK` to undo it.
  DuckLake bases are different: the lake side commits on its own and needs autocommit (no
  surrounding `BEGIN`).
- **Default scenarios read the live base.** If the base changes between the dry run and the
  merge, untouched rows show the new base values; rows the scenario changed are checked for
  conflicts at merge time (see "Conflicts").

## The playbook

### 1. Base data and the change to test

```sql
CREATE TABLE products (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, price DECIMAL(10,2) CHECK (price > 0));
INSERT INTO products VALUES (1, 'Widget', 9.99), (2, 'Gadget', 24.50), (3, 'Gizmo', 4.75), (4, 'Doohickey', 12.00);

-- staging data from an import, to be upserted
CREATE TABLE price_import (id INTEGER, name VARCHAR, price DECIMAL(10,2));
INSERT INTO price_import VALUES (2, 'Gadget', 26.00), (3, 'Gizmo', 4.75), (5, 'Thingamajig', 7.50);
```

### 2. Branch and run the change there

```sql
CALL scenario_create('price_import_2026_03', 'upsert price_import, retire Doohickey');
ATTACH 'price_import_2026_03' AS dry (TYPE scenario);

MERGE INTO dry.products p
USING price_import s ON p.id = s.id
WHEN MATCHED AND p.price <> s.price THEN UPDATE SET price = s.price
WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.name, s.price);

DELETE FROM dry.products WHERE name = 'Doohickey';
```

The base's `NOT NULL`, `CHECK`, `PRIMARY KEY` and `UNIQUE` constraints apply to the branch's
merged state, so a change that would break them fails here, in the dry run.

### 3. Validate the result like any table

```sql
SELECT count(*) AS products, min(price) AS min_price, max(price) AS max_price FROM dry.products;
```

### 4. Review the diff

```sql
SELECT * FROM scenario_diff_summary('price_import_2026_03');
```

```sql
SELECT id, change_type, column_name, old_value, new_value
FROM scenario_diff('price_import_2026_03', 'products')
ORDER BY id;
```

### 5. Preview and merge

```sql
SELECT * FROM scenario_merge_preview('price_import_2026_03') ORDER BY key;
```

Columns: `table_name`, `key` (the key as text), `action` (`insert` / `update` / `delete`),
`conflict` (BOOLEAN). Then apply:

```sql
DETACH dry;
SELECT * FROM scenario_merge('price_import_2026_03');
```

`scenario_merge` returns one row: `tables_changed`, `rows_applied`, `conflicts_resolved`.
The base now holds the change:

```sql
SELECT * FROM products ORDER BY id;
```

The merged scenario is frozen with an empty delta; remove it when done:

```sql
CALL scenario_drop('price_import_2026_03');
```

### Merge inside a transaction (host database)

```sql
CALL scenario_create('retire_gizmo');
ATTACH 'retire_gizmo' AS rg (TYPE scenario);
DELETE FROM rg.products WHERE id = 3;
DETACH rg;

BEGIN;
SELECT * FROM scenario_merge('retire_gizmo');
SELECT count(*) FROM products;   -- check before anyone else sees it
ROLLBACK;                        -- or COMMIT
```

After the `ROLLBACK` the base and the scenario are both back to their state before the merge,
so the scenario can be merged again later.

### Discard instead

```sql
CALL scenario_create('bad_idea');
ATTACH 'bad_idea' AS bad (TYPE scenario);
UPDATE bad.products SET price = price * 100;
DETACH bad;
CALL scenario_drop('bad_idea');   -- the base was never touched
```

## Conflicts

A conflict means the base changed under the scenario in a way that collides with one of its
changes. Which ones are detected depends on the mode:

| Scenario | Conflict when |
|---|---|
| delta (default) | an `insert` whose key now exists in the base; an `update` whose key no longer exists |
| materialized / DuckLake (have a creation snapshot) | additionally *drift*: the base row of a key the scenario touched changed since the snapshot |

`on_conflict` decides what happens:

| `on_conflict` | Effect |
|---|---|
| `'abort'` (default) | the merge fails and changes nothing |
| `'ours'` | the scenario's version wins |
| `'theirs'` | the base's version wins for conflicting keys; the rest is applied |

```sql
-- doc-snippets: new session
CREATE TABLE stock (sku VARCHAR PRIMARY KEY, qty INTEGER);
INSERT INTO stock VALUES ('A', 10), ('B', 20);

CALL scenario_create('restock');
ATTACH 'restock' AS r (TYPE scenario);
INSERT INTO r.stock VALUES ('C', 5);
UPDATE r.stock SET qty = 25 WHERE sku = 'B';
DETACH r;

-- meanwhile someone adds 'C' to the real table
INSERT INTO stock VALUES ('C', 7);

SELECT key, action, conflict FROM scenario_merge_preview('restock') ORDER BY key;
```

`C` is flagged: the scenario inserts a key that now exists in the base. `'abort'` would fail;
choose who wins:

```sql
SELECT * FROM scenario_merge('restock', on_conflict := 'theirs');
SELECT * FROM stock ORDER BY sku;   -- B = 25 from the scenario, C = 7 kept from the base
```

## Keyless tables

Tables without a PRIMARY KEY (and without `key_columns :=`) merge as bags of rows: per
distinct row the net number of deleted copies is removed from the base and the net inserted
copies are added. `scenario_merge_preview` lists one row per copy with `key` = the whole row as
text, e.g. `(A, 1)`. Prefer declaring a key at `scenario_create` when the table has one
logically:

```sql
-- doc-snippets: new session
CREATE TABLE ledger (entry_no INTEGER, account VARCHAR, amount DECIMAL(12,2));
CALL scenario_create('reclass', key_columns := MAP {'ledger': ['entry_no']});
```

## DuckLake bases

A scenario over a DuckLake catalog (`base := 'lake'`) merges into the lake:

- needs autocommit (no surrounding `BEGIN`): the lake commit happens in its own transaction;
- the lake commit is tagged (`commit_extra_info`), so if the host-side bookkeeping fails after
  the lake commit, re-running `scenario_merge` finishes it without applying anything twice;
- drift is checked against the scenario's pinned snapshot.

```sql
INSTALL ducklake;
ATTACH 'ducklake:lake.ducklake' AS lake (DATA_PATH 'lake_files/');
CREATE TABLE lake.orders (id INTEGER, status VARCHAR);
INSERT INTO lake.orders VALUES (1, 'open'), (2, 'open');

CALL scenario_create('close_orders', base := 'lake', key_columns := MAP {'orders': ['id']});
ATTACH 'close_orders' AS co (TYPE scenario);
UPDATE co.orders SET status = 'closed' WHERE id = 1;
DETACH co;

SELECT * FROM scenario_merge('close_orders');
```

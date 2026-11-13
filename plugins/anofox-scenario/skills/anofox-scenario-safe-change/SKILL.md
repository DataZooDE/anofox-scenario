---
name: anofox-scenario-safe-change
description: >
  Dry-run a data change in an anofox_scenario branch, review exactly what it would do,
  then apply it to the real tables or throw it away. Use when the user wants to test an
  ETL step, migration, bulk UPDATE/DELETE/MERGE, price change or data fix before applying
  it, asks for a "dry run" or "preview", wants to apply (merge) a scenario or a chosen
  plan variant to the real data, or needs to resolve merge conflicts. Main tools:
  scenario_merge_preview, scenario_merge with on_conflict := 'abort' | 'ours' | 'theirs',
  and scenario_drop to discard.
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — Safe Change Playbook (dry-run, review, merge)

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0

Run the change against a branch, check the result, read the diff, preview the merge, then
apply it in one step — or drop the branch and nothing ever happened. For error messages
and edge cases see `anofox-scenario-reference`.

## Critical gotchas

- **Ask before anything irreversible.** Show the user the diff summary, the merge preview and
  its conflict count, then ask before running `scenario_merge`, before choosing `'ours'` or
  `'theirs'`, and before `scenario_drop`. A committed merge has no undo; on a host database
  run it inside `BEGIN ... COMMIT` so the result can be checked first. A DuckLake merge commits
  on its own and cannot be rolled back.
- **Preview first.** `scenario_merge_preview(s)` lists every action the merge would take and
  flags conflicts, with no side effects. `scenario_merge` with the default
  `on_conflict := 'abort'` fails on the first conflict and changes nothing.
- **A merge is final for that scenario.** On success the scenario is frozen, `merged_at` is
  set and its delta is empty. A second merge fails (`already merged back into the base`).
  Drop it afterwards, or create a new scenario for the next change.
- **Merge refuses while branches exist** and while the scenario is frozen
  (`Unfreeze it before merging`). Drop the branches first; a branch that was itself merged
  still counts until it is dropped. A merged scenario cannot be unfrozen.
- **Merging a branch applies its parent's changes too** (a branch starts from its parent's
  state). See "Merging a branch".
- **Foreign keys are checked at merge time, not inside the scenario**, and the whole merge
  rolls back on a violation. Tables are applied one after another with immediate checks, so
  a scenario that inserts a new parent *and* a new child row referencing it currently fails
  to merge (issue #54). Add the new parent rows to the base (or merge them in a scenario of
  their own) first.
- **Host-database merges run in your transaction.** Wrap `scenario_merge` in
  `BEGIN ... COMMIT` to check the result before it becomes visible, or `ROLLBACK` to undo it.
  DuckLake bases are different: the lake side commits on its own and needs autocommit (no
  surrounding `BEGIN`).
- **Default (delta) scenarios do not detect base edits to rows they changed.** If someone
  edits a row in the base after the scenario updated it, the merge writes the scenario's whole
  row back and the base edit is lost, even with `'abort'`; the preview shows no conflict
  (issue #53). Merge right after the dry run, or create the scenario with
  `mode := 'materialized'` when the base may change in between: materialized and DuckLake
  scenarios flag such rows as conflicts. Rows the scenario did not touch keep any base change.

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
SELECT * FROM scenario_merge('price_import_2026_03');
```

`scenario_merge` returns one row: `tables_changed`, `rows_applied`, `conflicts_resolved`.
The base now holds the change:

```sql
SELECT * FROM products ORDER BY id;
```

The merged scenario is frozen with an empty delta; remove it when done (`DETACH` is needed
before a drop, not before a merge):

```sql
DETACH dry;
CALL scenario_drop('price_import_2026_03');
```

### Merge inside a transaction (host database)

```sql
CALL scenario_create('retire_gizmo');
ATTACH 'retire_gizmo' AS rg (TYPE scenario);
DELETE FROM rg.products WHERE id = 3;

BEGIN;
SELECT * FROM scenario_merge('retire_gizmo');
SELECT count(*) FROM products;   -- 3: check before anyone else sees it
ROLLBACK;                        -- or COMMIT
SELECT count(*) FROM products;   -- 4 again: the merge is undone
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
| delta (default) | an `insert` whose key now exists in the base; an `update` whose key no longer exists. **Not detected:** a base edit to a row the scenario updated; the scenario's row overwrites it (issue #53) |
| materialized / DuckLake (have a creation snapshot) | additionally *drift*: the base row of a key the scenario touched changed since the snapshot |

`on_conflict` decides what happens:

| `on_conflict` | Effect |
|---|---|
| `'abort'` (default) | the merge fails and changes nothing |
| `'ours'` | the scenario's version wins: a conflicting insert replaces the base row, and an update of a row the base deleted inserts it again |
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

The undetected case in a delta scenario, for comparison (the base's `name` edit is lost):

```sql
-- doc-snippets: new session
CREATE TABLE item (id INTEGER PRIMARY KEY, name VARCHAR, price INTEGER);
INSERT INTO item VALUES (1, 'old name', 10);
CALL scenario_create('reprice');
ATTACH 'reprice' AS rp (TYPE scenario);
UPDATE rp.item SET price = 11;
UPDATE item SET name = 'new name';                    -- base edit after the dry run
SELECT key, action, conflict FROM scenario_merge_preview('reprice');   -- conflict = false
SELECT * FROM scenario_merge('reprice');
SELECT * FROM item;                                   -- (1, 'old name', 11)
```

With `mode := 'materialized'` the same preview reports `conflict = true`.

## Merging a branch

To apply one variant out of several (see `anofox-scenario-what-if`), merge the scenario
that holds the full intended state. A branch made with `from_scenario` contains its parent's
changes, so merging the branch applies both. Afterwards drop the branch, then its
ancestors: their changes are already in the base, and merging them again would report their
inserts as conflicts.

```sql
-- doc-snippets: new session
CREATE TABLE plan (id INTEGER PRIMARY KEY, v INTEGER);
INSERT INTO plan VALUES (1, 1), (2, 2);
CALL scenario_create('base_case');
ATTACH 'base_case' AS bc (TYPE scenario);
UPDATE bc.plan SET v = 10 WHERE id = 1;
CALL scenario_create('chosen', from_scenario := 'base_case');
ATTACH 'chosen' AS ch (TYPE scenario);
UPDATE ch.plan SET v = 20 WHERE id = 2;

SELECT * FROM scenario_merge('chosen');    -- rows_applied = 2: the parent's change and its own
SELECT * FROM plan ORDER BY id;            -- (1, 10), (2, 20)

DETACH ch;
CALL scenario_drop('chosen');              -- the parent stays blocked until its branches are dropped
DETACH bc;
CALL scenario_drop('base_case');
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

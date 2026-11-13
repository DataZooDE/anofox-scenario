---
name: anofox-scenario-safe-change
description: >
  Try a data change on a sandbox copy of DuckDB tables with the anofox_scenario extension,
  review exactly what it would change, then apply it to the real tables, throw it away, or
  undo it later. Use for a dry run or preview of a bulk UPDATE / DELETE / MERGE, an ETL
  step, a backfill, a price change or a data fix; "test it on a copy / staging version
  first"; applying (merging) a scenario or a chosen plan variant to the real data; merge
  conflicts; undoing or rolling back a change applied by a scenario merge; SQL that already
  uses (TYPE scenario) or scenario_merge. Row data only: schema migrations (ADD COLUMN, new
  tables) cannot run inside a scenario because DDL is rejected; this skill says what to do
  instead. Not for comparing plan variants (anofox-scenario-what-if) or keeping approved,
  signed-off versions (anofox-scenario-snapshot-audit).
version: 2026.11.13
user-invocable: false
---

# Anofox Scenario — Safe Change Playbook (dry run, review, merge, undo)

**Extension:** `anofox_scenario` v2026.11.13 | **DuckDB:** v1.5.5+ and v2.0

Run the change in a scenario, check it, read the diff, preview the merge, then apply it in
one step, or drop the scenario and nothing happened. Errors, transactions and edge cases:
`anofox-scenario-reference`. Record, undo, keyless, DuckLake and schema changes:
[recipes.md](recipes.md).

## Ask first

Before `scenario_merge`, before choosing `'ours'` or `'theirs'`, and before `scenario_drop`
or `scenario_unfreeze`: show the user the summary, the preview counts and the conflicts, name
the scenario and what it will do to which tables, and wait for a yes (unless they already
asked for exactly that). A committed merge has no undo button; see "Undo" in
[recipes.md](recipes.md).

## Before creating the scenario

Decide these first; changing them later means a new scenario.

1. **Schema change?** `ALTER` / `CREATE` inside a scenario fails with `DDL operations are not
   permitted in scenarios`. For a migration that adds a column and backfills it, see
   "Schema changes" in [recipes.md](recipes.md); tell the user this before starting.
2. **Base:** the host database (default) or a DuckLake catalog (`base := 'lake'`, merge
   rules in [recipes.md](recipes.md)).
3. **Row identity:** does every table you change have a PRIMARY KEY? For a keyless table with
   a logical key, declare it now: `key_columns := MAP {'ledger': ['entry_no']}`. Keyless
   merges detect fewer base edits.
4. **Will the base change before you merge?** A default (delta) scenario flags base edits to
   rows it touched; the exceptions are in the reference skill ("Base edits a merge does not
   detect"). If they matter, merge right after the dry run.

## Critical gotchas

- **Preview first.** `scenario_merge_preview(s)` lists every action and flags conflicts, with
  no side effects. The default `on_conflict := 'abort'` fails on the first conflict and
  changes nothing.
- **A successful merge closes the scenario:** frozen, `merged_at` set (`scenario_list()`),
  delta empty, a second merge fails. Its diff is empty afterwards, so save a record first.
- **An empty plan is a no-op:** if the preview lists no rows, `scenario_merge` returns
  `0, 0, 0` and the scenario stays open and unfrozen, `merged_at` NULL.
- **Merge refuses while branches exist or while frozen.** A merged branch counts until it is
  dropped.
- **Merging a branch applies its parent's changes too.**
- **Foreign keys are checked at merge time**, not in the scenario; the whole merge rolls back on
  a violation.
- **Transactions:** a host-base merge runs in your transaction; DuckLake needs autocommit. The
  rules live once in the reference skill ("Transactions").

## The playbook

### 1. Base data and the change to test

```sql
CREATE TABLE products (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, price DECIMAL(10,2) CHECK (price > 0));
INSERT INTO products VALUES (1, 'Widget', 9.99), (2, 'Gadget', 24.50), (3, 'Gizmo', 4.75), (4, 'Doohickey', 12.00);

-- staging data from an import, to be upserted
CREATE TABLE price_import (id INTEGER, name VARCHAR, price DECIMAL(10,2));
INSERT INTO price_import VALUES (2, 'Gadget', 26.00), (3, 'Gizmo', 4.75), (5, 'Thingamajig', 7.50);
```

### 2. Create a scenario and run the change there

```sql
CALL scenario_create('price_import_2026_03', 'upsert price_import, retire Doohickey');
ATTACH 'price_import_2026_03' AS dry (TYPE scenario);

MERGE INTO dry.products p
USING price_import s ON p.id = s.id
WHEN MATCHED AND p.price <> s.price THEN UPDATE SET price = s.price
WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.name, s.price);

DELETE FROM dry.products WHERE name = 'Doohickey';
```

The base's `NOT NULL`, `CHECK`, `PRIMARY KEY` and `UNIQUE` constraints apply to the merged
state, so a change that would break them fails here, in the dry run.

### 3. Validate the result like any table

```sql
SELECT count(*) AS products, min(price) AS min_price, max(price) AS max_price FROM dry.products;
```

### 4. Review: counts first, then details

Per table, then per action and conflict (cheap, however large the change):

```sql
SELECT * FROM scenario_diff_summary('price_import_2026_03');
```

```sql
SELECT table_name, action, conflict, count(*) AS n
FROM scenario_merge_preview('price_import_2026_03')
GROUP BY ALL ORDER BY ALL;
```

Then details, bounded: conflicts first, a sample of the rest. A diff has one row per changed
column, a keyless preview one row per copy; list everything only for small changes.

```sql
SELECT * FROM scenario_merge_preview('price_import_2026_03') WHERE conflict LIMIT 100;
```

```sql
SELECT id, change_type, column_name, old_value, new_value
FROM scenario_diff('price_import_2026_03', 'products')
ORDER BY id
LIMIT 100;
```

### 5. What are you approving?

Put this to the user before merging:

- [ ] the scenario name, and for a branch, that its parents' changes are included;
- [ ] per table: rows inserted / updated / deleted (step 4);
- [ ] conflicts: none, or which rows and which policy (`'ours'` / `'theirs'`) the user chose;
- [ ] whether any table is keyless or the scenario is a branch of a materialized parent
      (base edits not fully detected, see the reference);
- [ ] that a record is saved for undo ([recipes.md](recipes.md), "Keep a record").

### 6. Merge

```sql
SELECT * FROM scenario_merge('price_import_2026_03');
```

It returns `tables_changed`, `rows_applied`, `conflicts_resolved`. The base now holds the
change, and the scenario shows `merged_at`:

```sql
SELECT * FROM products ORDER BY id;
SELECT name, frozen, merged_at IS NOT NULL AS merged FROM scenario_list();
```

Remove it when done, after asking (`DETACH` is needed before a drop, not before a merge):

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

After the `ROLLBACK` the scenario can be merged again later.

### Discard instead

```sql
CALL scenario_create('bad_idea');
ATTACH 'bad_idea' AS bad (TYPE scenario);
UPDATE bad.products SET price = price * 100;
DETACH bad;
CALL scenario_drop('bad_idea');   -- the base was never touched
```

## Conflicts

| Conflict | When |
|---|---|
| insert over an existing key | the scenario inserts a key that now exists in the base |
| update of a vanished row | the scenario updates a row the base has deleted since |
| *drift* | the base row of a key the scenario updated or deleted changed since the scenario's reference: its copy (materialized), the pinned snapshot (DuckLake), or the row at the scenario's first touch (delta) |
| keyless copy count | keyless table, delta scenario: the base holds fewer copies of a row than the scenario deletes (the base edited or deleted it) |

| `on_conflict` | Effect |
|---|---|
| `'abort'` (default) | the merge fails and changes nothing |
| `'ours'` | the scenario wins: a conflicting insert replaces the base row, an update of a deleted row inserts it again; keyless: deletes the copies that remain |
| `'theirs'` | the base wins for conflicting keys; keyless: deletes nothing for that row; the rest is applied |

Keyless inserts have no key to conflict on and apply under every policy, so a keyless row the
scenario changed can end up in both versions.

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

`C` is flagged. After the user picks who wins:

```sql
SELECT * FROM scenario_merge('restock', on_conflict := 'theirs');
SELECT * FROM stock ORDER BY sku;   -- B = 25 from the scenario, C = 7 kept from the base
```

Drift: someone edits a row the scenario changed, after the dry run:

```sql
-- doc-snippets: new session
CREATE TABLE item (id INTEGER PRIMARY KEY, name VARCHAR, price INTEGER);
INSERT INTO item VALUES (1, 'old name', 10);
CALL scenario_create('reprice');
ATTACH 'reprice' AS rp (TYPE scenario);
UPDATE rp.item SET price = 11;
UPDATE item SET name = 'new name';                    -- base edit after the dry run
SELECT key, action, conflict FROM scenario_merge_preview('reprice');   -- conflict = true
SELECT * FROM scenario_merge('reprice', on_conflict := 'theirs');
SELECT * FROM item;                                   -- (1, 'new name', 10)
```

Policies decide per row, not per column: `'ours'` would write `(1, 'old name', 11)` and undo
the rename. To keep both edits, redo the change in a new scenario over the current base.

## Merging a branch

To apply one variant out of several (`anofox-scenario-what-if`), merge the scenario that holds
the full intended state: a branch contains its parent's changes. Afterwards drop the branch,
then its ancestors. Do not merge an ancestor again: its changes are in the base already, so
every one of them is a conflict (inserts as existing keys, updates and deletes as drift).

Worked example with the ancestor's preview: [recipes.md](recipes.md), "Merging a branch".

A branch of a *materialized* parent: base edits to rows it only inherited from the parent are
not detected (reference skill, "Base edits a merge does not detect").

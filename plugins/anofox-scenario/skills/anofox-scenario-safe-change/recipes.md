# Anofox Scenario — Safe change recipes

Companion to the `anofox-scenario-safe-change` skill: keeping a record of a merge, undoing
one, merging a branch, keyless tables, DuckLake bases, schema changes. Ask the user before
every merge, drop and base change below.

## Keep a record before merging

After a merge the scenario's delta is empty: `scenario_diff` returns nothing and only
`merged_at` in `scenario_list()` shows that it happened. Save what it will change, and the
base rows it will overwrite, before merging:

```sql
CREATE TABLE products (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, price DECIMAL(10,2));
INSERT INTO products VALUES (1, 'Widget', 9.99), (2, 'Gadget', 24.50), (3, 'Gizmo', 4.75), (4, 'Doohickey', 12.00);

CALL scenario_create('price_fix', 'Gadget +1.50, add Thing, retire Doohickey');
ATTACH 'price_fix' AS pf (TYPE scenario);
UPDATE pf.products SET price = 26.00 WHERE id = 2;
INSERT INTO pf.products VALUES (5, 'Thing', 7.50);
DELETE FROM pf.products WHERE id = 4;
```

One pair of tables per changed table (`scenario_diff_summary` lists them), in a schema of
their own:

```sql
CREATE SCHEMA IF NOT EXISTS merge_log;
CREATE TABLE merge_log.price_fix_products_diff AS
    SELECT current_timestamp AS logged_at, * FROM scenario_diff('price_fix', 'products');
CREATE TABLE merge_log.price_fix_products_before AS
    SELECT * FROM products WHERE id IN (SELECT id FROM merge_log.price_fix_products_diff);

SELECT * FROM scenario_merge('price_fix');
SELECT name, merged_at IS NOT NULL AS merged FROM scenario_list();
```

`merge_log` tables are ordinary base tables: delta scenarios can see them, and they travel
with the database file.

## Undo a committed merge

Before `COMMIT`, `ROLLBACK` is the undo (host base). After it, apply the reverse change the
same safe way: a new scenario that deletes the keys the merge added and restores the rows it
changed or removed, then preview and merge. Drift checks flag rows someone edited since.

```sql
CALL scenario_create('undo_price_fix', 'reverse price_fix');
ATTACH 'undo_price_fix' AS u (TYPE scenario);
DELETE FROM u.products
WHERE id IN (SELECT id FROM merge_log.price_fix_products_diff WHERE change_type = 'added');
INSERT OR REPLACE INTO u.products SELECT * FROM merge_log.price_fix_products_before;

SELECT key, action, conflict FROM scenario_merge_preview('undo_price_fix') ORDER BY key;
```

After the user confirms:

```sql
SELECT * FROM scenario_merge('undo_price_fix');
SELECT * FROM products ORDER BY id;   -- back to the four original rows
```

Without a saved record, a materialized scenario created right before the merge
(`mode := 'materialized'`, a full copy) holds the old rows; read them through its alias. A
keyless table has no key to restore by: save a full copy of it instead
(`CREATE TABLE merge_log.t_before AS SELECT * FROM t`).

## Merging a branch

The branch's merge applies its parent's change too; the parent, merged again, would conflict
on everything it changed:

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
SELECT key, action, conflict FROM scenario_merge_preview('base_case');   -- 1 | update | true

DETACH ch;
CALL scenario_drop('chosen');              -- the parent stays blocked until its branches are dropped
DETACH bc;
CALL scenario_drop('base_case');
```

## Keyless tables

Tables without a PRIMARY KEY or `key_columns` merge as bags: per distinct row the net number
of deleted copies is removed and the net inserted copies are added. The preview lists one row
per copy, `key` = the whole row as text. Prefer declaring a key at creation:

```sql
-- doc-snippets: new session
CREATE TABLE ledger (entry_no INTEGER, account VARCHAR, amount DECIMAL(12,2));
CALL scenario_create('reclass', key_columns := MAP {'ledger': ['entry_no']});
```

Without a key, a base edit to a row the scenario changed is a conflict only if the base no
longer holds the copies the scenario deletes:

```sql
-- doc-snippets: new session
CREATE TABLE tally (account VARCHAR, amount INTEGER);
INSERT INTO tally VALUES ('x', 1);
CALL scenario_create('fix_amount');
ATTACH 'fix_amount' AS fa (TYPE scenario);
UPDATE fa.tally SET amount = 2;
UPDATE tally SET account = 'y';           -- base edit after the dry run
SELECT key, action, conflict FROM scenario_merge_preview('fix_amount') ORDER BY ALL;
```

The delete of `(x, 1)` is a conflict, the insert of `(x, 2)` is not:

```sql
-- fails: the base changed underneath it
SELECT * FROM scenario_merge('fix_amount');
```

With `'theirs'` nothing is deleted for `(x, 1)` and the insert still applies, so both versions
remain; `'ours'` would delete the copies that remain (none here):

```sql
SELECT * FROM scenario_merge('fix_amount', on_conflict := 'theirs');
SELECT * FROM tally ORDER BY ALL;   -- (x, 2) and (y, 1)
```

Not detected: a delete that still finds enough copies (the base added copies, or removed
fewer than the scenario deletes).

## DuckLake bases

A scenario over a DuckLake catalog (`base := 'lake'`) merges into the lake:

- autocommit only (no surrounding `BEGIN`): the lake commits in its own transaction and cannot
  be rolled back;
- drift is checked against the scenario's pinned snapshot;
- the lake commit is tagged (`commit_extra_info`). If the host bookkeeping fails after the
  lake commit, the scenario still looks unmerged: **do not write to it**, rerun the same
  `scenario_merge`; it finds the tagged commit and finishes without applying anything twice.
  If the scenario was written to in between, the rerun refuses (`it was already applied to
  base catalog`): compare lake and scenario and merge the remainder by hand.

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
SELECT * FROM lake.orders ORDER BY id;
```

## Schema changes (migrations)

DDL inside a scenario is rejected:

```sql
-- doc-snippets: new session
CREATE TABLE orders (id INTEGER PRIMARY KEY, amount DECIMAL(10,2));
INSERT INTO orders VALUES (1, 100), (2, 250);
CALL scenario_create('backfill_tax');
ATTACH 'backfill_tax' AS bt (TYPE scenario);
```

```sql
-- fails: DDL operations are not permitted in scenarios
ALTER TABLE bt.orders ADD COLUMN tax DECIMAL(10,2);
```

Tell the user, then offer one of:

- **Test the whole migration on a copy of the database file** with plain DuckDB (no scenario).
- **Split it:** the additive schema part on the real table (a nullable `ADD COLUMN`: existing
  readers see NULL), the data part in a scenario, reviewed and merged as usual. The base change
  is real, so ask first. It is safe while scenarios hold changes to the table: every scenario
  gets the column, and rows it changed earlier show the column's default (NULL here).
  Dropping, renaming or retyping a column, renaming or dropping the table is refused while a
  scenario has unmerged changes to it (`has unmerged changes to it`): merge those first.

```sql
ALTER TABLE orders ADD COLUMN tax DECIMAL(10,2);         -- real table, after the user's yes
UPDATE bt.orders SET tax = round(amount * 0.19, 2);      -- backfill in the scenario
SELECT table_name, action, conflict, count(*) AS n FROM scenario_merge_preview('backfill_tax') GROUP BY ALL;
```

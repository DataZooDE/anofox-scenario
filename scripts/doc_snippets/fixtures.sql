-- Fixtures for scripts/check_doc_snippets.py, prepended to every executed block (after
-- the extension is loaded). Keep it to base tables that several pages use without
-- creating them; a page that creates its own tables does so in a visible block, because
-- readers need to see the schema too. Deterministic: no random(), no now()-dependent data.

-- forecast(id, region, qty): the table the README's opening example branches.
CREATE TABLE forecast (id INTEGER PRIMARY KEY, region VARCHAR, qty DOUBLE);
INSERT INTO forecast VALUES (1, 'US', 100), (2, 'US', 80), (3, 'EU', 120), (4, 'APAC', 60);

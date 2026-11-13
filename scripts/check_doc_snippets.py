#!/usr/bin/env python3
"""scripts/check_doc_snippets.py

Docs/skills SQL snippet + function-name checker for anofox_scenario. Ported from
anofox-forecast's checker of the same name and adapted to how scenario docs are written:
as stories (create a scenario, attach it, write, diff, merge) whose blocks depend on the
state the previous blocks left behind.

Usage:
    python3 scripts/check_doc_snippets.py [FILE ...]
    python3 scripts/check_doc_snippets.py --changed-since REF
    python3 scripts/check_doc_snippets.py --out PATH [FILE ...]
    python3 scripts/check_doc_snippets.py --with-ext ducklake,anofox_forecast [--ext-dir DIR]
    python3 scripts/check_doc_snippets.py --show-output FILE   # print each block's output

Default scan (no FILE args): README.md, every docs/**/*.md and every
plugins/anofox-scenario/skills/**/*.md. Positional FILE args restrict the scan to those
files; `--changed-since REF` scans only files changed relative to REF (a full scan when
src/ or the support files changed, because error texts and fixtures are shared).

Execution model ("session replay"). Each ```sql block runs in a fresh in-memory DuckDB
(./build/release/duckdb, cwd and HOME a throwaway temp dir, autoinstall off) as:

    LOAD <extensions>; LOAD <built extension>; <sandbox>; fixtures.sql;
    <every earlier PASS block of the same session>; <this block>

Sandbox: after the loads, file access is limited to the throwaway directory
(allowed_directories + enable_external_access = false) and the configuration is locked, so
a block cannot read or write outside it, install or load anything, or undo the sandbox.
Lines starting with `.` (CLI dot-commands such as .shell) fail the block. Doc content may
still come from an untrusted PR: run the gate without secrets in the environment.

A session starts at the top of each file and restarts at any block whose first line is
`-- doc-snippets: new session`, or that is preceded by the (invisible when rendered) line
`<!-- doc-snippets: new session -->`. So a block may use the scenarios, attachments and tables
created by the blocks above it, exactly as a reader following the page would. Blocks that
are expected to fail, synopses and skipped blocks are never replayed.

Status per block:
  PASS            ran without error
  FAIL            errored (or an expected error did not happen / had the wrong text)
  EXPECTED-ERROR  listed in scripts/doc_snippets/expected_errors.txt and failed as listed.
                  Entry format: <file>TAB<first SQL line>[TAB<substring of the error>].
                  With a substring, the error output must contain it: this pins the error
                  texts the docs quote, so rewording an error fails the gate.
  SYNOPSIS        listed in scripts/doc_snippets/synopsis.txt (signature/placeholder text)
  SKIP-EXTERNAL   needs the network or another extension: the block contains a URL, or
                  INSTALL <ext> / `ducklake:` / a first line `-- requires: <ext>` for an
                  extension not enabled with --with-ext. With --with-ext the listed
                  extensions are loaded from --ext-dir (default: ~/.duckdb/extensions; install
                  them there first), and `INSTALL <ext>` lines for them become no-ops.

Output pinning: scripts/doc_snippets/expected_output.txt, <file>TAB<first SQL line>TAB
<lines>, where <lines> is `\n`-separated. Each line must be a substring of a line of the
block's own output (-list mode: `a|b|c`), in that order. Use it for examples whose point is
a result (conflict policy, diff direction, rollback), not just that they run.

Versions: plugin.json `version` must equal every SKILL.md `version:` and appear in each
skill's `**Extension:** `anofox_scenario` v<version>` banner.

Function names: every `scenario_[a-z_]+` identifier in a SKILL.md frontmatter, and every
one inside a backtick span or before `(` anywhere, must be a registered function
(`duckdb_functions()`) or be listed in scripts/doc_snippets/allowed_names.txt (column and
table names such as `scenario_id`).

Error texts: in any Markdown table whose first header cell is `Error contains`, every
backtick span of the first column is an error text the docs promise. Split at `…` (the
placeholder for a name), each fragment must occur in a C++ string literal under src/ or
duckdb/src/ (adjacent literals joined, printf specifiers matching `…`), so a reworded or
removed error fails the gate. Texts quoted from other DuckDB versions (not in this
duckdb/src checkout) are listed in scripts/doc_snippets/foreign_error_texts.txt. The same check applies to prose quotes of the form
"fails with `...`", "fails (`...`)", "error `...`" outside code blocks (`...` and `…` are
placeholders).

On a full scan, entries in the support files that no longer match anything are STALE.

Writes a TSV (status, file, line, first SQL line, detail) to --out (default
build/doc-snippets.tsv) and prints a summary. Exit 0 clean, 1 on any FAIL or STALE, 2 if
the duckdb binary or the extension is missing.

Requires a release build. CI runs it in the DuckLake integration job
(.github/workflows/MainDistributionPipeline.yml) with --with-ext ducklake,anofox_forecast;
locally run `GEN=ninja make release && make check-docs` in every PR that touches README.md,
docs/, a skill or an error message.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = subprocess.run(
    ["git", "rev-parse", "--show-toplevel"], cwd=SCRIPT_DIR, capture_output=True, text=True, check=True
).stdout.strip()

SUPPORT_DIR = os.path.join(SCRIPT_DIR, "doc_snippets")
FIXTURES_FILE = os.path.join(SUPPORT_DIR, "fixtures.sql")
EXPECTED_ERRORS_FILE = os.path.join(SUPPORT_DIR, "expected_errors.txt")
SYNOPSIS_FILE = os.path.join(SUPPORT_DIR, "synopsis.txt")
ALLOWED_NAMES_FILE = os.path.join(SUPPORT_DIR, "allowed_names.txt")
EXPECTED_OUTPUT_FILE = os.path.join(SUPPORT_DIR, "expected_output.txt")
FOREIGN_ERRORS_FILE = os.path.join(SUPPORT_DIR, "foreign_error_texts.txt")
SELF_EXT = "anofox_scenario"  # the built extension: always loaded, INSTALL/LOAD of it are no-ops
PLUGIN_JSON = os.path.join(REPO_ROOT, "plugins", "anofox-scenario", ".claude-plugin", "plugin.json")
DEFAULT_EXT_DIR = os.path.join(os.path.expanduser("~"), ".duckdb", "extensions")
BLOCK_MARKER = "__doc_snippet_block_output__"
DUCKDB_BIN = os.path.join(REPO_ROOT, "build", "release", "duckdb")
EXTENSION_FILE = os.path.join(
    REPO_ROOT, "build", "release", "extension", "anofox_scenario", "anofox_scenario.duckdb_extension"
)
DEFAULT_OUT = os.path.join(REPO_ROOT, "build", "doc-snippets.tsv")

DEFAULT_SCAN_DIRS = ["docs", os.path.join("plugins", "anofox-scenario", "skills")]

FENCE_RE = re.compile(r"^```[ \t]*([A-Za-z0-9_+-]*)[ \t]*$")
URL_RE = re.compile(r"https?://|s3://|gcs://|az://", re.IGNORECASE)
INSTALL_RE = re.compile(r"^\s*INSTALL\s+([A-Za-z0-9_]+)\b[^;]*;", re.IGNORECASE | re.MULTILINE)
DUCKLAKE_RE = re.compile(r"ducklake:", re.IGNORECASE)
DOT_COMMAND_RE = re.compile(r"^\s*\.[A-Za-z]", re.MULTILINE)
PROSE_ERROR_RE = re.compile(
    r"\b(?:fails?|failing|errors?|refuses?|rejected|throws?)\s+(?:with\s+)?\(?`([^`]+)`", re.IGNORECASE)
REQUIRES_RE = re.compile(r"\A\s*--\s*requires:\s*(\S+)", re.IGNORECASE)
NEW_SESSION_RE = re.compile(r"\A\s*--\s*doc-snippets:\s*new session", re.IGNORECASE)
NEW_SESSION_HTML_RE = re.compile(r"^\s*<!--\s*doc-snippets:\s*new session\s*-->\s*$", re.IGNORECASE)

IDENT_RE = re.compile(r"(?<![A-Za-z0-9_.])(scenario_[a-z0-9_]+)")
BACKTICK_IDENT_RE = re.compile(r"`(scenario_[a-z0-9_]+)`")
CALL_IDENT_RE = re.compile(r"(?<![A-Za-z0-9_.])(scenario_[a-z0-9_]+)\s*\(")


def read_text(abs_path):
    with open(abs_path, encoding="utf-8") as f:
        return f.read()


def changed_md_files(ref):
    result = subprocess.run(
        ["git", "diff", "--name-only", ref, "--", "README.md"] + DEFAULT_SCAN_DIRS,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    files = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return [f for f in files if f.endswith(".md") and os.path.exists(os.path.join(REPO_ROOT, f))]


def shared_inputs_changed(ref):
    result = subprocess.run(
        ["git", "diff", "--name-only", ref, "--", "src", "scripts", "plugins/anofox-scenario/.claude-plugin"],
        cwd=REPO_ROOT, capture_output=True, text=True, check=True,
    )
    return bool(result.stdout.strip())


def default_scan_files():
    files = []
    if os.path.exists(os.path.join(REPO_ROOT, "README.md")):
        files.append("README.md")
    for scan_dir in DEFAULT_SCAN_DIRS:
        abs_dir = os.path.join(REPO_ROOT, scan_dir)
        if not os.path.isdir(abs_dir):
            continue
        for root, dirs, fnames in os.walk(abs_dir):
            dirs.sort()
            for fn in sorted(fnames):
                if fn.endswith(".md"):
                    files.append(os.path.relpath(os.path.join(root, fn), REPO_ROOT))
    return files


def extract_sql_blocks(text):
    """Return list of (block_text, start_line_idx, new_session) for ```sql fenced blocks."""
    lines = text.splitlines()
    blocks = []
    i = 0
    n = len(lines)
    while i < n:
        m = FENCE_RE.match(lines[i].strip())
        if m and m.group(1).lower() == "sql":
            start = i
            prev = i - 1
            while prev >= 0 and not lines[prev].strip():
                prev -= 1
            new_session = prev >= 0 and bool(NEW_SESSION_HTML_RE.match(lines[prev]))
            body_lines = []
            i += 1
            while i < n and lines[i].strip() != "```":
                body_lines.append(lines[i])
                i += 1
            blocks.append(("\n".join(body_lines), start, new_session))
        i += 1
    return blocks


def first_non_comment_line(block_text):
    for line in block_text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("--"):
            return stripped
    for line in block_text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def load_lines(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [line.rstrip("\n") for line in f if line.strip() and not line.startswith("#")]


def load_expected_errors(path):
    """<file>TAB<first line>[TAB<error substring>] -> {key: substring-or-None}."""
    entries = {}
    for line in load_lines(path):
        parts = line.split("\t")
        key = "\t".join(parts[:2])
        entries[key] = parts[2] if len(parts) > 2 and parts[2] else None
    return entries


def load_allowed_names(path):
    names = {}
    for line in load_lines(path):
        parts = line.split("\t", 1)
        names[parts[0].strip()] = parts[1].strip() if len(parts) > 1 else ""
    return names


def error_text(stderr, stdout):
    return "\n".join(s for s in (stderr.strip(), stdout.strip()) if s)


def first_error_line(text):
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:200]
    return ""


def terminate(sql_text):
    """Make sure the block's last statement is terminated so that the next one parses."""
    body = sql_text.rstrip()
    lines = body.splitlines()
    # a trailing `-- comment` line would swallow the appended ';'
    while lines and lines[-1].strip().startswith("--"):
        lines.pop()
    if lines and not lines[-1].rstrip().endswith(";"):
        return body + "\n;"
    return body


def sql_str(value):
    return "'" + value.replace("'", "''") + "'"


def run_sql(sql_text, timeout=120, exts=(), ext_dir=None):
    tmp_dir = tempfile.mkdtemp(prefix="doc_snippets_")
    try:
        env = dict(os.environ)
        env["HOME"] = tmp_dir
        prelude = ["SET autoinstall_known_extensions = false;", "SET autoload_known_extensions = false;"]
        if exts:
            prelude.append("SET extension_directory = %s;" % sql_str(ext_dir or DEFAULT_EXT_DIR))
            prelude += ["LOAD %s;" % e for e in exts]
        prelude += [
            "LOAD %s;" % sql_str(EXTENSION_FILE),
            "SET allowed_directories = [%s];" % sql_str(tmp_dir + os.sep),
            "SET enable_external_access = false;",
            "SET lock_configuration = true;",
        ]
        try:
            result = subprocess.run(
                [DUCKDB_BIN, "-unsigned", "-bail", "-list", ":memory:"],
                input="\n".join(prelude) + "\n" + sql_text + "\n",
                cwd=tmp_dir,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
            )
            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return 124, "", f"TIMEOUT after {timeout}s"
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def needed_extensions(block_text):
    """Extensions a block needs (None if it needs the network, which is never run)."""
    if URL_RE.search(block_text):
        return None
    exts = {m.group(1).lower() for m in INSTALL_RE.finditer(block_text)} - {SELF_EXT}
    if DUCKLAKE_RE.search(block_text):
        exts.add("ducklake")
    requires = REQUIRES_RE.search(block_text)
    if requires:
        exts.add(requires.group(1).lower())
    return exts


def neutralize_installs(block_text, enabled):
    """`INSTALL x ...;` for a preloaded extension becomes a comment (and `LOAD x;` too)."""
    enabled = set(enabled) | {SELF_EXT}
    def install(m):
        return "-- (preinstalled) " + m.group(0).strip() if m.group(1).lower() in enabled else m.group(0)
    text = INSTALL_RE.sub(install, block_text)
    load_re = re.compile(r"^\s*LOAD\s+([A-Za-z0-9_]+)\s*;", re.IGNORECASE | re.MULTILINE)
    return load_re.sub(lambda m: "-- (preloaded) " + m.group(0).strip()
                       if m.group(1).lower() in enabled else m.group(0), text)


def block_output_lines(stdout):
    lines = stdout.splitlines()
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == BLOCK_MARKER:
            return lines[i + 1:]
    return lines


def output_matches(out_lines, expected_lines):
    """Each expected line is a substring of an output line, in order. Returns the first miss."""
    pos = 0
    for want in expected_lines:
        while pos < len(out_lines) and want not in out_lines[pos]:
            pos += 1
        if pos == len(out_lines):
            return want
        pos += 1
    return None


def load_expected_output(path):
    entries = {}
    for line in load_lines(path):
        parts = line.split("\t")
        if len(parts) >= 3:
            entries["\t".join(parts[:2])] = parts[2].split("\\n")
    return entries


def check_versions():
    """FAIL rows when plugin.json, SKILL.md frontmatter and banners disagree."""
    import json
    rows = []
    if not os.path.exists(PLUGIN_JSON):
        return rows
    with open(PLUGIN_JSON) as f:
        version = json.load(f).get("version", "")
    skills_dir = os.path.join(REPO_ROOT, "plugins", "anofox-scenario", "skills")
    for root, _dirs, fnames in os.walk(skills_dir):
        for fn in fnames:
            if fn != "SKILL.md":
                continue
            rel = os.path.relpath(os.path.join(root, fn), REPO_ROOT)
            text = read_text(os.path.join(root, fn))
            m = re.search(r"^version:\s*(\S+)", text, re.MULTILINE)
            if not m or m.group(1) != version:
                rows.append(("FAIL", rel, "", "", f"frontmatter version {m.group(1) if m else '(none)'} != plugin.json {version}"))
            b = re.search(r"\*\*Extension:\*\*\s*`anofox_scenario`\s*v(\S+)", text)
            if not b or b.group(1) != version:
                rows.append(("FAIL", rel, "", "", f"banner version {b.group(1) if b else '(none)'} != plugin.json {version}"))
    return rows


def query_registered_function_names():
    rc, out, err = run_sql("SELECT DISTINCT function_name FROM duckdb_functions();")
    if rc != 0:
        print(f"ERROR: failed to query duckdb_functions(): {error_text(err, out)[:500]}", file=sys.stderr)
        sys.exit(2)
    return {line.strip() for line in out.splitlines() if line.strip()}


STRING_LITERAL_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ERROR_TABLE_HEADER_RE = re.compile(r"^\|\s*Error contains\s*\|", re.IGNORECASE)
PRINTF_RE = re.compile(r"%(?:[-+ 0#]*\d*(?:\.\d+)?(?:ll|l|z)?[sdiuf]|%)")

_corpus = None


def error_corpus():
    """All C++ string literals of the extension and of DuckDB, adjacent literals joined,
    printf specifiers replaced by the docs' placeholder."""
    global _corpus
    if _corpus is not None:
        return _corpus
    parts = []
    for root_dir in (os.path.join(REPO_ROOT, "src"), os.path.join(REPO_ROOT, "duckdb", "src")):
        for root, _, fnames in os.walk(root_dir):
            for fn in fnames:
                if not fn.endswith((".cpp", ".hpp", ".h")):
                    continue
                try:
                    code = read_text(os.path.join(root, fn))
                except UnicodeDecodeError:
                    continue
                for m in re.finditer(r'"(?:[^"\\\n]|\\.)*"(?:\s*"(?:[^"\\\n]|\\.)*")*', code):
                    literal = "".join(STRING_LITERAL_RE.findall(m.group(0)))
                    parts.append(PRINTF_RE.sub("\u2026", literal.replace('\\"', '"').replace("\\'", "'")))
    _corpus = "\n".join(parts)
    return _corpus


def collect_error_texts(text):
    """(error text, line_no) for the first-column backtick spans of `Error contains` tables."""
    found = []
    in_table = False
    for i, line in enumerate(text.splitlines()):
        if ERROR_TABLE_HEADER_RE.match(line):
            in_table = True
            continue
        if in_table:
            if not line.strip().startswith("|"):
                in_table = False
                continue
            cells = line.split("|")
            if len(cells) < 2 or set(cells[1].strip()) <= set("-: "):
                continue
            for span in re.findall(r"`([^`]+)`", cells[1]):
                found.append((span, i + 1))
    return found


def collect_prose_error_texts(text):
    """(error text, line_no) for "fails with `...`"-style quotes outside code fences."""
    found = []
    in_fence = False
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence or ERROR_TABLE_HEADER_RE.match(line):
            continue
        # join with the next line so a quote split by a line break is still seen
        joined = line + " " + (lines[i + 1] if i + 1 < len(lines) else "")
        for m in PROSE_ERROR_RE.finditer(joined):
            if m.start() < len(line):
                found.append((" ".join(m.group(1).split()), i + 1))
    return found


def error_text_known(span):
    if span in set(load_lines(FOREIGN_ERRORS_FILE)):
        return True
    corpus = error_corpus()
    frags = [f for f in span.replace("...", "\u2026").split("\u2026") if f]
    pattern = "[^\\n]*?".join(re.escape(f) for f in frags)
    return re.search(pattern, corpus) is not None


def collect_name_mentions(rel_path, text):
    mentions = []
    lines = text.splitlines()
    frontmatter_end = 0
    is_skill = rel_path.endswith("SKILL.md")
    if is_skill and lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                frontmatter_end = i
                break
        for i in range(1, frontmatter_end):
            for m in IDENT_RE.finditer(lines[i]):
                mentions.append((m.group(1), i + 1))
    for i, line in enumerate(lines):
        if is_skill and i < frontmatter_end:
            continue
        for m in BACKTICK_IDENT_RE.finditer(line):
            mentions.append((m.group(1), i + 1))
        for m in CALL_IDENT_RE.finditer(line):
            mentions.append((m.group(1), i + 1))
    return mentions


def main():
    args = sys.argv[1:]
    files_arg = []
    changed_since = None
    out_path = DEFAULT_OUT
    enabled_exts = []
    ext_dir = None
    show_output = False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--changed-since":
            i += 1
            if i >= len(args):
                print("ERROR: --changed-since requires a REF argument", file=sys.stderr)
                return 1
            changed_since = args[i]
        elif a == "--with-ext":
            i += 1
            if i >= len(args):
                print("ERROR: --with-ext requires a comma-separated list", file=sys.stderr)
                return 1
            enabled_exts = [e.strip().lower() for e in args[i].split(",") if e.strip()]
        elif a == "--show-output":
            show_output = True
        elif a == "--ext-dir":
            i += 1
            if i >= len(args):
                print("ERROR: --ext-dir requires a PATH argument", file=sys.stderr)
                return 1
            ext_dir = args[i]
        elif a == "--out":
            i += 1
            if i >= len(args):
                print("ERROR: --out requires a PATH argument", file=sys.stderr)
                return 1
            out_path = args[i]
        else:
            files_arg.append(a)
        i += 1

    for path, what in ((DUCKDB_BIN, "duckdb binary"), (EXTENSION_FILE, "extension")):
        if not os.path.exists(path):
            print(f"ERROR: {what} not found at {path} -- build it first (GEN=ninja make release)", file=sys.stderr)
            return 2

    is_full_scan = not files_arg and not changed_since
    if files_arg:
        files = files_arg
    elif changed_since:
        files = changed_md_files(changed_since)
        if shared_inputs_changed(changed_since):
            files = default_scan_files()
    else:
        files = default_scan_files()

    fixtures = read_text(FIXTURES_FILE) if os.path.exists(FIXTURES_FILE) else ""
    expected_errors = load_expected_errors(EXPECTED_ERRORS_FILE)
    synopsis_keys = set(load_lines(SYNOPSIS_FILE))
    allowed_names = load_allowed_names(ALLOWED_NAMES_FILE)
    expected_output = load_expected_output(EXPECTED_OUTPUT_FILE)
    registered_names = query_registered_function_names()

    rows = []
    counts = {k: 0 for k in ("PASS", "FAIL", "EXPECTED-ERROR", "SYNOPSIS", "SKIP-EXTERNAL", "STALE")}
    all_block_keys = set()
    all_mentioned_names = set()
    seen_unknown = set()

    for row in check_versions():
        rows.append(row)
        counts["FAIL"] += 1

    for rel_path in files:
        abs_path = os.path.join(REPO_ROOT, rel_path)
        if not os.path.exists(abs_path):
            continue
        text = read_text(abs_path)

        for name, line_no in collect_name_mentions(rel_path, text):
            all_mentioned_names.add(name)
            if name in registered_names or name in allowed_names or (rel_path, name) in seen_unknown:
                continue
            seen_unknown.add((rel_path, name))
            rows.append(("FAIL", rel_path, str(line_no), "", f"unknown function name {name}"))
            counts["FAIL"] += 1

        for span, line_no in collect_error_texts(text) + collect_prose_error_texts(text):
            if not error_text_known(span):
                rows.append(("FAIL", rel_path, str(line_no), "", f"error text not found in src/ or duckdb/src/: {span}"))
                counts["FAIL"] += 1

        session = []  # earlier PASS blocks of the current session
        for block_text, start_line, new_session in extract_sql_blocks(text):
            first_line = first_non_comment_line(block_text)
            key = f"{rel_path}\t{first_line}"
            all_block_keys.add(key)
            if new_session or NEW_SESSION_RE.search(block_text):
                session = []

            needs = needed_extensions(block_text)
            if DOT_COMMAND_RE.search(block_text):
                status, detail = "FAIL", "CLI dot-commands are not allowed in doc blocks"
            elif needs is None:
                status, detail = "SKIP-EXTERNAL", "network block, not executed"
            elif not needs <= set(enabled_exts):
                status, detail = "SKIP-EXTERNAL", "needs " + ", ".join(sorted(needs - set(enabled_exts)))
            elif key in synopsis_keys:
                status, detail = "SYNOPSIS", ""
            else:
                runnable = neutralize_installs(block_text, set(enabled_exts))
                session_exts = set(needs)
                for b in session:
                    session_exts |= needed_extensions(b) or set()
                sql = "\n".join([fixtures] + [terminate(neutralize_installs(b, set(enabled_exts))) for b in session]
                                + ["SELECT '%s' AS marker;" % BLOCK_MARKER, runnable])
                rc, stdout, stderr = run_sql(sql, exts=sorted(session_exts), ext_dir=ext_dir)
                err = error_text(stderr, stdout)
                if key in expected_errors:
                    want = expected_errors[key]
                    if rc == 0:
                        status, detail = "FAIL", "expected error but passed"
                    elif want and want not in err:
                        status, detail = "FAIL", f"error text changed, expected '{want}': {first_error_line(err)}"
                    else:
                        status, detail = "EXPECTED-ERROR", ""
                elif rc == 0:
                    status, detail = "PASS", ""
                    if show_output:
                        print(f"--- {rel_path}:{start_line + 1} {first_line[:80]}")
                        print("\n".join(block_output_lines(stdout)))
                    if key in expected_output:
                        miss = output_matches(block_output_lines(stdout), expected_output[key])
                        if miss is not None:
                            status, detail = "FAIL", f"output does not contain '{miss}' (in order)"
                    if status == "PASS":
                        session.append(block_text)
                else:
                    status, detail = "FAIL", first_error_line(err)

            rows.append((status, rel_path, str(start_line + 1), first_line[:100], detail))
            counts[status] += 1

    if is_full_scan:
        for key in sorted(set(expected_errors) | synopsis_keys | set(expected_output)):
            if key not in all_block_keys:
                rows.append(("STALE", key.split("\t", 1)[0], "", "", f"stale support-file entry: {key}"))
                counts["STALE"] += 1
        for name in sorted(allowed_names):
            if name in registered_names or name not in all_mentioned_names:
                rows.append(("STALE", "scripts/doc_snippets/allowed_names.txt", "", "",
                             f"{name} is registered or no longer mentioned"))
                counts["STALE"] += 1

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        for row in rows:
            f.write("\t".join(row) + "\n")
    for row in rows:
        if row[0] in ("FAIL", "STALE"):
            print("\t".join(row))

    print("check_doc_snippets: " + ", ".join(f"{v} {k}" for k, v in counts.items()))
    return 1 if (counts["FAIL"] or counts["STALE"]) else 0


if __name__ == "__main__":
    sys.exit(main())

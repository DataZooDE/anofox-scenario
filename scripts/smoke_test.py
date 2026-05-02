#!/usr/bin/env python3
"""
Smoke test for the anofox_scenario DuckDB extension.

Installs the matching DuckDB Python package (same version the extension was built against)
into a project-local venv, loads the locally-built extension artifact, and exercises the
core scenario lifecycle — the same path a real user would take.
"""
import argparse
import importlib.metadata
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PROJ_DIR = Path(__file__).resolve().parent.parent
VENV_DIR = PROJ_DIR / ".smoke_venv"
DEFAULT_EXTENSION_PATH = (
    PROJ_DIR / "build" / "release" / "extension" / "anofox_scenario" / "anofox_scenario.duckdb_extension"
)


def get_duckdb_version_from_submodule() -> str:
    # Try exact tag first (clean checkout)
    result = subprocess.run(
        ["git", "describe", "--tags", "--exact-match", "HEAD"],
        cwd=PROJ_DIR / "duckdb",
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        return result.stdout.strip().lstrip("v")
    # Fall back: strip -N-gSHA suffix from describe output
    result = subprocess.run(
        ["git", "describe", "--tags"],
        cwd=PROJ_DIR / "duckdb",
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip().lstrip("v").split("-")[0]


def ensure_venv() -> None:
    """If not already inside a venv, create one under .smoke_venv/ and re-exec there."""
    if sys.prefix != sys.base_prefix:
        return  # Already inside a venv

    venv_python = VENV_DIR / "bin" / "python"
    if not VENV_DIR.exists():
        print(f"Creating venv at {VENV_DIR} ...")
        subprocess.check_call([sys.executable, "-m", "venv", str(VENV_DIR)])

    os.execv(str(venv_python), [str(venv_python)] + sys.argv)


def ensure_duckdb_version(version: str) -> None:
    """Install the exact duckdb version; re-exec the process if we just installed it."""
    try:
        installed = importlib.metadata.version("duckdb")
        if installed == version:
            return
        print(f"duckdb {installed} installed, need {version} — reinstalling ...")
    except importlib.metadata.PackageNotFoundError:
        print(f"duckdb not installed — installing {version} ...")

    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", f"duckdb=={version}", "--quiet"],
    )
    # Re-exec so the newly installed package is importable in this process
    os.execv(sys.executable, [sys.executable] + sys.argv)


def run_test(extension_path: Path) -> None:
    import duckdb  # noqa: PLC0415 — intentionally deferred until after version install

    print(f"duckdb {duckdb.__version__}")
    print(f"extension: {extension_path}")

    with tempfile.TemporaryDirectory() as tmpdir:
        conn = duckdb.connect(
            os.path.join(tmpdir, "smoke.duckdb"),
            config={"allow_unsigned_extensions": True},
        )

        conn.execute(f"LOAD '{extension_path}'")

        # Base table that scenarios will branch off
        conn.execute(
            "CREATE TABLE products (id INTEGER PRIMARY KEY, name VARCHAR, price DECIMAL(10,2))"
        )
        conn.execute("INSERT INTO products VALUES (1, 'Widget', 9.99), (2, 'Gadget', 19.99)")

        # scenario_create
        ok = conn.execute(
            "SELECT scenario_create('smoke_test', 'Smoke test scenario')"
        ).fetchone()[0]
        assert ok is True, f"scenario_create failed: {ok!r}"

        # scenario_list
        rows = conn.execute("SELECT scenario_name, status FROM scenario_list()").fetchall()
        assert rows == [("smoke_test", "active")], f"scenario_list unexpected: {rows!r}"

        # scenario_drop
        ok = conn.execute("SELECT scenario_drop('smoke_test')").fetchone()[0]
        assert ok is True, f"scenario_drop failed: {ok!r}"

        # Verify clean state
        rows = conn.execute("SELECT scenario_name FROM scenario_list()").fetchall()
        assert rows == [], f"Expected empty list after drop, got {rows!r}"

        conn.close()

    print("Smoke test PASSED")


def main() -> None:
    parser = argparse.ArgumentParser(description="Smoke test for anofox_scenario extension")
    parser.add_argument(
        "--extension",
        type=Path,
        default=DEFAULT_EXTENSION_PATH,
        help="Path to the .duckdb_extension artifact (default: build/release artifact)",
    )
    parser.add_argument(
        "--duckdb-version",
        default=os.environ.get("DUCKDB_VERSION"),
        help="DuckDB version to install via pip (default: read from duckdb/ submodule git tag)",
    )
    args = parser.parse_args()

    if not args.extension.exists():
        print(f"ERROR: Extension artifact not found: {args.extension}")
        print("Run 'make release' first to build the extension.")
        sys.exit(1)

    version = args.duckdb_version or get_duckdb_version_from_submodule()
    print(f"DuckDB version: {version}")

    ensure_venv()
    ensure_duckdb_version(version)
    run_test(args.extension.resolve())


if __name__ == "__main__":
    main()

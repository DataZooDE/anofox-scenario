PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quack
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Smoke test: installs the matching DuckDB version via pip, loads the built artifact, and
# verifies core functionality using the same path a real user would take.
# Downloads the real DuckDB release — not build leftovers — so this catches signing/loading issues.
.PHONY: smoke_test
smoke_test: release
	python3 scripts/smoke_test.py
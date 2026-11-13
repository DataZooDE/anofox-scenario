PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quack
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Docs/skills gate: runs every ```sql block in README.md, docs/ and the Claude Code plugin
# skills against the release build, and resolves every scenario_* name. Needs
# `GEN=ninja make release` first. CI runs it in the DuckLake integration job with
# DuckLake and anofox_forecast blocks enabled; `make check-docs-all` does the same locally
# (INSTALL ducklake; INSTALL anofox_forecast FROM community; in ./build/release/duckdb first).
.PHONY: check-docs check-docs-all
check-docs:
	python3 scripts/check_doc_snippets.py
check-docs-all:
	python3 scripts/check_doc_snippets.py --with-ext ducklake,anofox_forecast

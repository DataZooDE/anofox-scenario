PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quack
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Docs/skills gate: runs every ```sql block in README.md, docs/ and the Claude Code plugin
# skills against the release build, and resolves every scenario_* name. Needs
# `GEN=ninja make release` first, so it is not part of the extension-ci-tools pipeline;
# run it before every release and in PRs that touch docs or skills.
.PHONY: check-docs
check-docs:
	python3 scripts/check_doc_snippets.py

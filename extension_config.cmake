# This file is included by DuckDB's build system. It specifies which extension to load

# DuckDB v1.5.x builds core as C++11 (strong out-of-line definitions of its
# `static constexpr` members). On GCC, C++17 TUs that ODR-use those members emit
# them as STB_GNU_UNIQUE symbols, which collide with core's strong definitions at
# link time -- e.g. duckdb::BufferedFileWriter::DEFAULT_OPEN_FLAGS in the core
# `plan_serializer` tool, and LogicalType::VARCHAR/BIGINT/TIMESTAMP in this
# extension. This config is included (duckdb/CMakeLists.txt, via
# extension_build_tools.cmake) BEFORE DuckDB add_subdirectory()'s src/ and tools/,
# so setting the flag here reaches DuckDB core + tool targets as well as this
# extension. -fno-gnu-unique emits those members as weak instead, resolving
# cleanly against core's definitions. GCC-only (Clang/MSVC do not use GNU_UNIQUE).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-gnu-unique")
endif()

# Extension from this repo
# DuckDB v1.5.6 stamps the short commit SHA instead of the release tag: its duckdb_extension_generate_version
# passes --match '${VERSIONING_TAG_MATCH}' with literal single quotes, so no tag ever matches. Pass the version
# explicitly when HEAD is on a vX.Y.Z tag (2.0.x detects it correctly too; this keeps both lines identical).
set(ANOFOX_SCENARIO_EXT_VERSION "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "v*.*.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE ANOFOX_SCENARIO_GIT_RESULT
        OUTPUT_VARIABLE ANOFOX_SCENARIO_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ANOFOX_SCENARIO_GIT_RESULT EQUAL 0 AND ANOFOX_SCENARIO_GIT_DESCRIBE MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[a-zA-Z0-9\\.]+)?$")
        set(ANOFOX_SCENARIO_EXT_VERSION "${ANOFOX_SCENARIO_GIT_DESCRIBE}")
    endif()
endif()
if(ANOFOX_SCENARIO_EXT_VERSION STREQUAL "")
    duckdb_extension_load(anofox_scenario
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        LOAD_TESTS
    )
else()
    message(STATUS "anofox_scenario extension version ${ANOFOX_SCENARIO_EXT_VERSION}")
    duckdb_extension_load(anofox_scenario
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        EXTENSION_VERSION ${ANOFOX_SCENARIO_EXT_VERSION}
        LOAD_TESTS
    )
endif()

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)

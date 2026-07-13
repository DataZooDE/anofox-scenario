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
duckdb_extension_load(anofox_scenario
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)

cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME anofox_scenario)


set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/anofox_scenario_extension.cpp
    src/metadata_store.cpp
    src/scenario_manager.cpp
    src/snapshot_manager.cpp
    src/protocol_manager.cpp
    src/delta_storage_engine.cpp
    src/comparison_engine.cpp
    src/ddl_blocker.cpp
    src/catalog/scenario_registry.cpp
    src/catalog/scenario_catalog.cpp
    src/catalog/scenario_schema_entry.cpp
    src/catalog/scenario_table_entry.cpp
    src/catalog/scenario_scan.cpp
    src/catalog/scenario_delta.cpp
    src/catalog/scenario_insert.cpp
    src/catalog/scenario_update_delete.cpp
    src/catalog/scenario_transaction.cpp
    src/catalog/scenario_storage_extension.cpp
    src/lifecycle/scenario_lifecycle.cpp
    src/lifecycle/scenario_diff.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME anofox_scenario)


set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

# Add PostHog telemetry library (requires OpenSSL, not available for WASM/MinGW/cross-compile)
set(TELEMETRY_SUPPORTED TRUE)
if(DEFINED ENV{DUCKDB_WASM_PLATFORM} OR "${DUCKDB_EXPLICIT_PLATFORM}" MATCHES "^wasm" OR EMSCRIPTEN)
    set(TELEMETRY_SUPPORTED FALSE)
    message(STATUS "WASM build - building without PostHog telemetry")
elseif(MINGW)
    set(TELEMETRY_SUPPORTED FALSE)
    message(STATUS "MinGW build - building without PostHog telemetry")
elseif(APPLE AND DEFINED OSX_BUILD_ARCH AND NOT "${OSX_BUILD_ARCH}" STREQUAL "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    set(TELEMETRY_SUPPORTED FALSE)
    message(STATUS "Cross-compiling on macOS (${OSX_BUILD_ARCH} on ${CMAKE_HOST_SYSTEM_PROCESSOR}) - building without PostHog telemetry")
endif()

if(TELEMETRY_SUPPORTED)
    # Static OpenSSL avoids runtime dependencies on libssl.so.1.1 (Linux) and
    # libssl-3-x64.dll / libcrypto-3-x64.dll (Windows). On Windows this requires
    # `openssl` in vcpkg.json so the x64-windows-static-release triplet supplies
    # static libs built with /MT (matching the extension's CRT).
    set(OPENSSL_USE_STATIC_LIBS ON)
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        set(DUCKDB_SRC_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include")
        set(DUCKDB_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party")
        add_subdirectory(posthog-telemetry)
        include_directories(posthog-telemetry/include)
        add_definitions(-DHAS_POSTHOG_TELEMETRY)
    else()
        message(STATUS "OpenSSL not found - building without PostHog telemetry")
    endif()
endif()

set(EXTENSION_SOURCES
    src/anofox_scenario_extension.cpp
    src/metadata_store.cpp
    src/scenario_manager.cpp
    src/snapshot_manager.cpp
    src/protocol_manager.cpp
    src/delta_storage_engine.cpp
    src/comparison_engine.cpp
    src/ddl_blocker.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Telemetry library + OpenSSL (only when OpenSSL was found)
set(TELEMETRY_LIBS "")
if(TELEMETRY_SUPPORTED AND OPENSSL_FOUND)
    set(TELEMETRY_LIBS posthog_telemetry ${OPENSSL_LIBRARIES})
endif()

# Link telemetry and platform-specific libs into the extension targets
if(TELEMETRY_LIBS)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${EXTENSION_NAME} ${TELEMETRY_LIBS} pthread dl m)
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${TELEMETRY_LIBS} pthread dl m)
    elseif(WIN32)
        target_link_libraries(${EXTENSION_NAME} ${TELEMETRY_LIBS} bcrypt)
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${TELEMETRY_LIBS} bcrypt)
    elseif(APPLE)
        target_link_libraries(${EXTENSION_NAME} ${TELEMETRY_LIBS} "-framework CoreFoundation" "-framework SystemConfiguration")
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${TELEMETRY_LIBS} "-framework CoreFoundation" "-framework SystemConfiguration")
    else()
        target_link_libraries(${EXTENSION_NAME} ${TELEMETRY_LIBS})
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${TELEMETRY_LIBS})
    endif()
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

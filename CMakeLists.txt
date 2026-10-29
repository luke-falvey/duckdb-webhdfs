cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME webhdfs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# No third-party dependencies: HTTP comes from httpfs at runtime (through
# DuckDB's HTTPUtil), JSON from the yyjson copy in DuckDB core, and Kerberos
# from the system GSSAPI library, loaded with dlopen when first needed.
set(EXTENSION_SOURCES
    src/webhdfs_client.cpp
    src/webhdfs_config.cpp
    src/webhdfs_extension.cpp
    src/webhdfs_filesystem.cpp
    src/webhdfs_glob.cpp
    src/webhdfs_hadoop_config.cpp
    src/webhdfs_http.cpp
    src/webhdfs_json.cpp
    src/webhdfs_kerberos.cpp
    src/webhdfs_secret.cpp
    src/webhdfs_url.cpp
    src/webhdfs_walk.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(NOT WIN32)
  target_link_libraries(${EXTENSION_NAME} ${CMAKE_DL_LIBS})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ${CMAKE_DL_LIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

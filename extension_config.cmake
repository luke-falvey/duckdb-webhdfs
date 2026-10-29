# This file is included by DuckDB's build system. It specifies which extensions to build.

# This repo's extension.
duckdb_extension_load(webhdfs
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
duckdb_extension_statically_link(webhdfs)

# httpfs supplies every HTTP request webhdfs makes, at runtime, through DuckDB's
# HTTPUtil interface. webhdfs never links it. It is built here only so the test
# runner and the bundled shell have it; the commit is the one DuckDB v2.0.0 pins
# (duckdb/.github/config/extensions/httpfs.cmake). The clang-tidy build skips it:
# it runs no tests, and CI's tidy job has no curl to build httpfs with.
if(NOT CLANG_TIDY)
  duckdb_extension_load(httpfs
      GIT_URL https://github.com/duckdb/duckdb-httpfs
      GIT_TAG 77392a72077a85897db2519fae11e2c0da972b8a
      INCLUDE_DIR src/include
  )
  duckdb_extension_statically_link(httpfs)
endif()

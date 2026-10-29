PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=webhdfs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
# Unit tests that need neither a DuckDB build nor a cluster: URLs, globs, Hadoop config and JSON parsing.
test_unit:
	mkdir -p build/unit_test
	$(CXX) -std=c++17 -O1 -Wall -Wextra -Isrc/include -Iduckdb/third_party/yyjson/include \
		src/webhdfs_url.cpp src/webhdfs_glob.cpp src/webhdfs_hadoop_config.cpp src/webhdfs_json.cpp \
		duckdb/third_party/yyjson/yyjson.cpp test/cpp/unit_tests.cpp -o build/unit_test/webhdfs_unit_tests
	./build/unit_test/webhdfs_unit_tests

# Integration tests: start the Docker clusters, run test/sql/integration, tear down (needs Docker and `make`).
test_integration:
	test/scripts/run_webhdfs_tests.sh

.PHONY: test_unit test_integration

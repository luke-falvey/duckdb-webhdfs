#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Register the `webhdfs` secret type (CREATE SECRET (TYPE webhdfs, ...))
void RegisterWebHdfsSecretType(ExtensionLoader &loader);

} // namespace duckdb

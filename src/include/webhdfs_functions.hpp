#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class WebHdfsFileSystem;

//! Register the webhdfs_* metadata SQL functions. `fs` is owned by the database's virtual file system and outlives
//! the functions.
void RegisterWebHdfsFunctions(ExtensionLoader &loader, WebHdfsFileSystem &fs);

} // namespace duckdb

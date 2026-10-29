#define DUCKDB_EXTENSION_MAIN

#include "webhdfs_extension.hpp"

#include "duckdb/main/database.hpp"
#include "webhdfs_config.hpp"
#include "webhdfs_filesystem.hpp"
#include "webhdfs_http.hpp"
#include "webhdfs_secret.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	// Step 1 of making sure httpfs is there: try to load (and install) it now, so its settings and secret types
	// work right after LOAD webhdfs. Requests check again before they are sent (WebHdfsHttp::GetHttpUtil).
	WebHdfsHttp::TryLoadHttpfs(db);

	WebHdfsSettings::Register(db);
	RegisterWebHdfsSecretType(loader);

	auto &fs = db.GetFileSystem();
	fs.RegisterSubSystem(make_uniq<WebHdfsFileSystem>(db));
}

void WebhdfsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string WebhdfsExtension::Name() {
	return "webhdfs";
}

std::string WebhdfsExtension::Version() const {
#ifdef EXT_VERSION_WEBHDFS
	return EXT_VERSION_WEBHDFS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(webhdfs, loader) {
	duckdb::LoadInternal(loader);
}
}

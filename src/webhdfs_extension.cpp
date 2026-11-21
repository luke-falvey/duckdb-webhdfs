#define DUCKDB_EXTENSION_MAIN

#include "webhdfs_extension.hpp"

#include "duckdb/main/database.hpp"
#include "webhdfs_config.hpp"
#include "webhdfs_filesystem.hpp"
#include "webhdfs_functions.hpp"
#include "webhdfs_http.hpp"
#include "webhdfs_secret.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	// Step 1 of making sure httpfs is there: load an httpfs that is already available (no install, so
	// LOAD webhdfs never waits on the network), so its settings and secret types work right after LOAD webhdfs.
	// Requests check again before they are sent and may install it then (WebHdfsHttp::GetHttpUtil).
	WebHdfsHttp::TryLoadHttpfs(db);

	auto webhdfs_fs = make_uniq<WebHdfsFileSystem>(db);
	// the virtual file system owns the filesystem for the database's lifetime; the functions and settings borrow it
	auto &fs_ref = *webhdfs_fs;
	// registered first: the settings hook keeps a reference, which is only safe once the VFS owns the filesystem
	db.GetFileSystem().RegisterSubSystem(std::move(webhdfs_fs));
	WebHdfsSettings::Register(db, [&fs_ref](idx_t bytes) { fs_ref.SetSmallFileCacheSize(bytes); });
	// a SET from another connection may reach the hook first: the seed then leaves its value alone
	fs_ref.SeedSmallFileCacheSize(WebHdfsSettings::LoadSmallFileCacheSize(db));
	RegisterWebHdfsSecretType(loader);
	RegisterWebHdfsFunctions(loader, fs_ref);
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

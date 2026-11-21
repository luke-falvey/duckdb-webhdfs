#pragma once

// All HTTP goes through DuckDB's HTTPUtil, which the httpfs extension implements at runtime. webhdfs never links
// httpfs: it loads it (two steps, see TryLoadHttpfs / GetHttpUtil) and then talks to the HTTPUtil interface only.

#include "duckdb.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/http/http_util.hpp"

#include <functional>

namespace duckdb {

struct WebHdfsHttpResult {
	//! HTTP status, or 0 when no response arrived (connection refused, timeout, ...)
	int status = 0;
	//! Response body, except for 2xx responses streamed into a sink
	string body;
	//! Transport error text when status == 0
	string request_error;
	//! Location header (redirects), if any
	string location;
	//! WWW-Authenticate header, if any
	string www_authenticate;
	//! Bytes delivered to the sink
	idx_t bytes_streamed = 0;

	bool Ok() const {
		return status >= 200 && status < 300;
	}
	bool IsRedirect() const {
		return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
	}
};

//! Receives a streamed 2xx body.
struct WebHdfsBodySink {
	//! Called before each attempt's body (an internal retry restarts the body from its first byte)
	std::function<void()> reset;
	//! Called with each body chunk; return false to stop the transfer
	std::function<bool(const_data_ptr_t data, idx_t len)> write;

	explicit operator bool() const {
		return bool(write);
	}
};

class WebHdfsHttp {
public:
	//! Step 1 (when webhdfs loads): load an already available httpfs if autoload_known_extensions allows it (no
	//! install, so LOAD webhdfs never waits on the network). Never throws; failure is reported by GetHttpUtil.
	static void TryLoadHttpfs(DatabaseInstance &db) noexcept;
	//! Step 2 (before every request): the HTTP provider. While httpfs is missing and autoloading is enabled, an
	//! available copy is loaded; installing it (if autoinstall_known_extensions allows) is tried once per database.
	//! Throws MissingExtensionException, with the cause of the failed load, if no provider is available.
	static HTTPUtil &GetHttpUtil(DatabaseInstance &db);

	//! HTTP parameters for requests made on behalf of `original_url` (a webhdfs:// URL): httpfs settings and any
	//! `http` secret scoped to that URL apply, and HTTPUtil's file access check is made against it as a regular file.
	//! For `access_type` FILE_TYPE_DIR, HTTPUtil's check is skipped: it can't check a directory as one, and
	//! WebHdfsFileSystem::CreateClient has just checked the same URL as a directory. The skip only holds while every
	//! request's access path is `original_url`: params built for another URL (a DataNode leg, O9) must not take it.
	static unique_ptr<HTTPParams> CreateParams(DatabaseInstance &db, optional_ptr<FileOpener> opener,
	                                           const string &original_url,
	                                           FileType access_type = FileType::FILE_TYPE_REGULAR);

	static WebHdfsHttpResult Get(DatabaseInstance &db, HTTPParams &params, const string &url,
	                             const vector<std::pair<string, string>> &headers,
	                             const WebHdfsBodySink &sink = WebHdfsBodySink());
	static WebHdfsHttpResult Put(DatabaseInstance &db, HTTPParams &params, const string &url,
	                             const vector<std::pair<string, string>> &headers, const_data_ptr_t data, idx_t len);
	static WebHdfsHttpResult Post(DatabaseInstance &db, HTTPParams &params, const string &url,
	                              const vector<std::pair<string, string>> &headers, const_data_ptr_t data, idx_t len);
	static WebHdfsHttpResult Delete(DatabaseInstance &db, HTTPParams &params, const string &url,
	                                const vector<std::pair<string, string>> &headers);
};

} // namespace duckdb

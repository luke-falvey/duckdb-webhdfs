#pragma once

// The WebHDFS REST client: NameNode selection and failover, redirects to DataNodes, authentication and error
// mapping. It is the only code that sends requests (through WebHdfsHttp, i.e. httpfs).

#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "webhdfs_config.hpp"
#include "webhdfs_http.hpp"
#include "webhdfs_json.hpp"
#include "webhdfs_url.hpp"

#include <atomic>
#include <chrono>
#include <deque>

namespace duckdb {

//! Long-lived state of one webhdfs authority (scheme://host[:port]), shared by all requests to it.
class WebHdfsEndpoint {
public:
	explicit WebHdfsEndpoint(string key_p) : key(std::move(key_p)) {
	}

	struct NameNodeInfo {
		string state = "unknown";
		string last_error;
		idx_t requests = 0;
	};

	struct Snapshot {
		string key;
		vector<std::pair<string, NameNodeInfo>> namenodes;
		string active;
		idx_t datanode_requests = 0;
		int batch_listing = -1;
		string auth_mode;
		string namenode_source;
		string secret_name;
	};

	const string key;

	//! Index into `namenodes` to try first: the last NameNode that answered as active, else 0
	idx_t PreferredIndex(const vector<string> &namenodes);
	void RecordRequest(const string &namenode);
	void MarkActive(const string &namenode);
	void MarkFailed(const string &namenode, const string &state, const string &error);
	void RecordDataNodeRequest() {
		datanode_requests++;
	}
	void RecordConfig(const WebHdfsConfig &config);
	Snapshot GetSnapshot();

	//! LISTSTATUS_BATCH support: -1 unknown, 0 no, 1 yes
	std::atomic<int> batch_listing {-1};

	//! Delegation token fetched with Kerberos (empty if none / expired)
	string GetKerberosToken();
	void SetKerberosToken(const string &token);
	void ClearKerberosToken();
	//! The NameNode does not issue delegation tokens over HTTP (a gateway): SPNEGO on every request instead
	std::atomic<bool> spnego_every_request {false};

	//! DataNode redirects of earlier reads, for reads that skip the NameNode. Keys include the identity, the path
	//! and the file version, so a redirect is only reused by the same user for the same file contents.
	string GetRedirect(const string &key);
	void PutRedirect(const string &key, const string &url);
	void DropRedirect(const string &key, const string &url);

	//! Contents of small files read whole (keys as for redirects), so a second open of the same file version
	//! (DuckDB's readers open a file at bind time and again to scan it) costs no request
	shared_ptr<const string> GetSmallFile(const string &key);
	void PutSmallFile(const string &key, shared_ptr<const string> contents);
	static constexpr idx_t SMALL_FILE_CACHE_BYTES = idx_t(64) << 20;

private:
	mutex lock;
	vector<string> order;
	unordered_map<string, NameNodeInfo> namenodes;
	string active;
	std::atomic<idx_t> datanode_requests {0};
	string auth_mode;
	string namenode_source;
	string secret_name;
	string kerberos_token;
	std::chrono::steady_clock::time_point kerberos_token_expiry;
	static constexpr idx_t MAX_REDIRECTS = 4096;
	unordered_map<string, string> redirects;
	unordered_map<string, shared_ptr<const string>> small_files;
	//! insertion order, for evicting the oldest entries first
	std::deque<string> small_file_order;
	idx_t small_file_bytes = 0;
};

enum class WebHdfsErrorKind : uint8_t {
	NONE,
	NOT_FOUND,
	PERMISSION,
	ALREADY_EXISTS,
	STANDBY,
	RETRIABLE,
	UNSUPPORTED,
	INVALID_TOKEN,
	OTHER
};

//! Classify a failed response from its status and RemoteException body
WebHdfsErrorKind ClassifyWebHdfsError(const WebHdfsHttpResult &result, webhdfs::RemoteException &exception);

class WebHdfsClient {
public:
	WebHdfsClient(DatabaseInstance &db, optional_ptr<FileOpener> opener, webhdfs::WebHdfsUrl url, WebHdfsConfig config,
	              WebHdfsSettings settings, WebHdfsEndpoint &endpoint);

	const webhdfs::WebHdfsUrl &Url() const {
		return url;
	}
	const WebHdfsConfig &Config() const {
		return config;
	}
	const WebHdfsSettings &Settings() const {
		return settings;
	}
	WebHdfsEndpoint &Endpoint() {
		return endpoint;
	}
	//! scheme://authority<path>
	string UrlFor(const string &path) const;
	//! Key for per-file caches: identity (so cached data never crosses users), path and file version
	string CacheKey(const string &path, const string &version) const;

	//! GETFILESTATUS; false if the path does not exist
	bool TryGetFileStatus(const string &path, webhdfs::FileStatus &out);
	webhdfs::FileStatus GetFileStatus(const string &path);
	//! LISTSTATUS_BATCH (or LISTSTATUS on NameNodes without it); `callback` receives the entries page by page.
	//! A file path lists the file itself (with an empty path_suffix).
	void ListStatus(const string &path, const std::function<void(vector<webhdfs::FileStatus> &)> &callback);
	bool Mkdirs(const string &path, const string &permission = "");
	bool Delete(const string &path, bool recursive);
	bool Rename(const string &source, const string &destination);
	//! Any NameNode GET operation; returns the JSON body
	string GetJson(const string &op, const string &path, const vector<std::pair<string, string>> &params = {});
	//! A GET operation the NameNode answers by redirecting to a DataNode (GETFILECHECKSUM)
	string GetJsonViaDataNode(const string &op, const string &path,
	                          const vector<std::pair<string, string>> &params = {});

	//! Read [offset, offset + length) into `buffer` (OPEN). With a file version (e.g. "<mtime>:<length>"), the
	//! DataNode redirect of an earlier read of the same file version is reused.
	void ReadRange(const string &path, idx_t offset, idx_t length, data_ptr_t buffer, const string &version = "");
	//! CREATE a file with `data` (may be empty)
	void Create(const string &path, const_data_ptr_t data, idx_t len, bool overwrite);
	//! APPEND `data` to an existing file
	void Append(const string &path, const_data_ptr_t data, idx_t len);

	//! Throw the DuckDB exception for a failed response
	[[noreturn]] void ThrowError(const string &op, const string &path, const WebHdfsHttpResult &result);

private:
	WebHdfsHttpResult NameNodeCall(RequestType type, const string &op, const string &path,
	                               const vector<std::pair<string, string>> &params,
	                               const WebHdfsBodySink &sink = WebHdfsBodySink());
	WebHdfsHttpResult DataNodeCall(RequestType type, const string &url, const_data_ptr_t data, idx_t len,
	                               const WebHdfsBodySink &sink = WebHdfsBodySink());
	WebHdfsHttpResult Send(RequestType type, const string &url, const vector<std::pair<string, string>> &headers,
	                       const_data_ptr_t data, idx_t len, const WebHdfsBodySink &sink);
	//! The delegation token for Kerberos clusters (fetched once with SPNEGO), or "" in SPNEGO-every-request mode
	string KerberosDelegationToken();
	string FetchKerberosDelegationToken();
	vector<std::pair<string, string>> SpnegoHeaders(const string &url);
	bool CheckBoolean(const string &op, const string &path, const WebHdfsHttpResult &result);

private:
	DatabaseInstance &db;
	webhdfs::WebHdfsUrl url;
	WebHdfsConfig config;
	WebHdfsSettings settings;
	WebHdfsEndpoint &endpoint;
	unique_ptr<HTTPParams> params;
};

//! Epoch milliseconds to a DuckDB timestamp
timestamp_t WebHdfsTimestamp(int64_t epoch_ms);

} // namespace duckdb

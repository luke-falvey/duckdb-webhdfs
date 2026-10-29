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
};

//! A DataNode redirect remembered by an open file, for reads that skip the NameNode.
struct WebHdfsRedirectCache {
	mutex lock;
	string url;
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

	//! Read [offset, offset + length) into `buffer` (OPEN). With a cache, the DataNode redirect is reused.
	void ReadRange(const string &path, idx_t offset, idx_t length, data_ptr_t buffer,
	               optional_ptr<WebHdfsRedirectCache> cache);
	//! CREATE a file with `data` (may be empty)
	void Create(const string &path, const_data_ptr_t data, idx_t len, bool overwrite);
	//! APPEND `data` to an existing file
	void Append(const string &path, const_data_ptr_t data, idx_t len);

	//! Throw the DuckDB exception for a failed response
	[[noreturn]] void ThrowError(const string &op, const string &path, const WebHdfsHttpResult &result);

private:
	WebHdfsHttpResult NameNodeCall(RequestType type, const string &op, const string &path,
	                               vector<std::pair<string, string>> params,
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

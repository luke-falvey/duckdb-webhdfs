#include "webhdfs_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "webhdfs_kerberos.hpp"

#include <cstring>
#include <thread>

namespace duckdb {

using webhdfs::FileStatus;
using webhdfs::RemoteException;

timestamp_t WebHdfsTimestamp(int64_t epoch_ms) {
	return Timestamp::FromEpochMs(epoch_ms);
}

//===--------------------------------------------------------------------===//
// Endpoint
//===--------------------------------------------------------------------===//

idx_t WebHdfsEndpoint::PreferredIndex(const vector<string> &list) {
	lock_guard<mutex> guard(lock);
	for (idx_t i = 0; i < list.size(); i++) {
		if (list[i] == active) {
			return i;
		}
	}
	return 0;
}

void WebHdfsEndpoint::RecordRequest(const string &namenode) {
	lock_guard<mutex> guard(lock);
	auto entry = namenodes.find(namenode);
	if (entry == namenodes.end()) {
		order.push_back(namenode);
		entry = namenodes.emplace(namenode, NameNodeInfo()).first;
	}
	entry->second.requests++;
}

void WebHdfsEndpoint::MarkActive(const string &namenode) {
	lock_guard<mutex> guard(lock);
	active = namenode;
	auto &info = namenodes[namenode];
	info.state = "active";
}

void WebHdfsEndpoint::MarkFailed(const string &namenode, const string &state, const string &error) {
	lock_guard<mutex> guard(lock);
	auto &info = namenodes[namenode];
	info.state = state;
	info.last_error = error;
	if (active == namenode) {
		active.clear();
	}
}

void WebHdfsEndpoint::RecordConfig(const WebHdfsConfig &config) {
	lock_guard<mutex> guard(lock);
	auth_mode = AuthModeToString(config.auth);
	namenode_source = config.namenode_source;
	secret_name = config.secret_name;
	for (auto &nn : config.namenodes) {
		if (namenodes.find(nn) == namenodes.end()) {
			order.push_back(nn);
			namenodes.emplace(nn, NameNodeInfo());
		}
	}
}

WebHdfsEndpoint::Snapshot WebHdfsEndpoint::GetSnapshot() {
	lock_guard<mutex> guard(lock);
	Snapshot snapshot;
	snapshot.key = key;
	for (auto &nn : order) {
		snapshot.namenodes.emplace_back(nn, namenodes[nn]);
	}
	snapshot.active = active;
	snapshot.datanode_requests = datanode_requests.load();
	snapshot.batch_listing = batch_listing.load();
	snapshot.auth_mode = auth_mode;
	snapshot.namenode_source = namenode_source;
	snapshot.secret_name = secret_name;
	return snapshot;
}

string WebHdfsEndpoint::GetKerberosToken() {
	lock_guard<mutex> guard(lock);
	if (!kerberos_token.empty() && std::chrono::steady_clock::now() >= kerberos_token_expiry) {
		kerberos_token.clear();
	}
	return kerberos_token;
}

void WebHdfsEndpoint::SetKerberosToken(const string &token) {
	lock_guard<mutex> guard(lock);
	kerberos_token = token;
	// Hadoop's default renew interval is 24 hours; fetch a fresh token well before that
	kerberos_token_expiry = std::chrono::steady_clock::now() + std::chrono::hours(12);
}

void WebHdfsEndpoint::ClearKerberosToken() {
	lock_guard<mutex> guard(lock);
	kerberos_token.clear();
}

string WebHdfsEndpoint::GetRedirect(const string &key) {
	lock_guard<mutex> guard(lock);
	auto entry = redirects.find(key);
	return entry == redirects.end() ? string() : entry->second;
}

void WebHdfsEndpoint::PutRedirect(const string &key, const string &url) {
	lock_guard<mutex> guard(lock);
	if (redirects.size() >= MAX_REDIRECTS) {
		redirects.clear();
	}
	redirects[key] = url;
}

void WebHdfsEndpoint::DropRedirect(const string &key, const string &url) {
	lock_guard<mutex> guard(lock);
	auto entry = redirects.find(key);
	if (entry != redirects.end() && entry->second == url) {
		redirects.erase(entry);
	}
}

//===--------------------------------------------------------------------===//
// Errors
//===--------------------------------------------------------------------===//

static bool Contains(const string &haystack, const char *needle) {
	return haystack.find(needle) != string::npos;
}

WebHdfsErrorKind ClassifyWebHdfsError(const WebHdfsHttpResult &result, RemoteException &exception) {
	if (result.Ok() || result.IsRedirect()) {
		return WebHdfsErrorKind::NONE;
	}
	exception = RemoteException();
	webhdfs::ParseRemoteException(result.body, exception);
	auto &name = exception.exception;
	auto &java = exception.java_class_name;
	auto &message = exception.message;
	if (name == "StandbyException" || name == "ObserverRetryOnActiveException" || Contains(java, "StandbyException") ||
	    Contains(message, "not supported in state standby")) {
		return WebHdfsErrorKind::STANDBY;
	}
	if (name == "FileNotFoundException" || Contains(java, "FileNotFoundException")) {
		return WebHdfsErrorKind::NOT_FOUND;
	}
	if (Contains(java, "InvalidToken") || name == "InvalidToken" ||
	    Contains(message, "token") && (Contains(message, "expired") || Contains(message, "can't be found"))) {
		return WebHdfsErrorKind::INVALID_TOKEN;
	}
	if (name == "AccessControlException" || name == "SecurityException" || name == "AuthorizationException" ||
	    Contains(java, "AccessControlException") || result.status == 401) {
		return WebHdfsErrorKind::PERMISSION;
	}
	if (name == "FileAlreadyExistsException" || Contains(java, "FileAlreadyExistsException")) {
		return WebHdfsErrorKind::ALREADY_EXISTS;
	}
	if (name == "RetriableException" || name == "SafeModeException" || Contains(java, "RetriableException") ||
	    Contains(java, "SafeModeException")) {
		return WebHdfsErrorKind::RETRIABLE;
	}
	if (name == "UnsupportedOperationException" ||
	    (name == "IllegalArgumentException" &&
	     (Contains(message, "No enum constant") || Contains(message, "Invalid value for webhdfs parameter \"op\"")))) {
		return WebHdfsErrorKind::UNSUPPORTED;
	}
	if (result.status == 404 && name.empty()) {
		return WebHdfsErrorKind::NOT_FOUND;
	}
	return WebHdfsErrorKind::OTHER;
}

//===--------------------------------------------------------------------===//
// Client
//===--------------------------------------------------------------------===//

WebHdfsClient::WebHdfsClient(DatabaseInstance &db_p, optional_ptr<FileOpener> opener, webhdfs::WebHdfsUrl url_p,
                             WebHdfsConfig config_p, WebHdfsSettings settings_p, WebHdfsEndpoint &endpoint_p)
    : db(db_p), url(std::move(url_p)), config(std::move(config_p)), settings(std::move(settings_p)),
      endpoint(endpoint_p) {
	params = WebHdfsHttp::CreateParams(db, opener, url.ToString());
	endpoint.RecordConfig(config);
}

string WebHdfsClient::UrlFor(const string &path) const {
	return url.Prefix() + path;
}

void WebHdfsClient::ThrowError(const string &op, const string &path, const WebHdfsHttpResult &result) {
	auto target = UrlFor(path);
	if (result.status == 0) {
		throw IOException("WebHDFS %s on \"%s\" failed: %s", op, target, result.request_error);
	}
	RemoteException exception;
	auto kind = ClassifyWebHdfsError(result, exception);
	string detail;
	if (!exception.exception.empty()) {
		detail = exception.exception + ": " + exception.message;
	} else if (!result.body.empty() && result.body.size() < 512) {
		detail = StringUtil::Replace(result.body, "\n", " ");
	} else {
		detail = "HTTP " + std::to_string(result.status);
	}
	switch (kind) {
	case WebHdfsErrorKind::NOT_FOUND:
		throw IOException("WebHDFS: no such file or directory: \"%s\"", target);
	case WebHdfsErrorKind::PERMISSION:
	case WebHdfsErrorKind::INVALID_TOKEN:
		throw PermissionException("WebHDFS %s on \"%s\" was denied: %s", op, target, detail);
	case WebHdfsErrorKind::ALREADY_EXISTS:
		throw IOException("WebHDFS %s on \"%s\" failed: the file already exists", op, target);
	case WebHdfsErrorKind::UNSUPPORTED:
		throw NotImplementedException("WebHDFS %s is not supported by the server for \"%s\": %s", op, target, detail);
	default:
		throw IOException("WebHDFS %s on \"%s\" failed (HTTP %d): %s", op, target, result.status, detail);
	}
}

WebHdfsHttpResult WebHdfsClient::Send(RequestType type, const string &request_url,
                                      const vector<std::pair<string, string>> &headers, const_data_ptr_t data,
                                      idx_t len, const WebHdfsBodySink &sink) {
	static const data_t EMPTY_BODY[1] = {0};
	if (!data) {
		// a null body makes curl read the request body from stdin
		data = EMPTY_BODY;
		len = 0;
	}
	switch (type) {
	case RequestType::GET_REQUEST:
		return WebHdfsHttp::Get(db, *params, request_url, headers, sink);
	case RequestType::PUT_REQUEST:
		return WebHdfsHttp::Put(db, *params, request_url, headers, data, len);
	case RequestType::POST_REQUEST:
		return WebHdfsHttp::Post(db, *params, request_url, headers, data, len);
	case RequestType::DELETE_REQUEST:
		return WebHdfsHttp::Delete(db, *params, request_url, headers);
	default:
		throw InternalException("unsupported WebHDFS request type");
	}
}

static string HostOf(const string &request_url) {
	auto sep = request_url.find("://");
	auto start = sep == string::npos ? 0 : sep + 3;
	auto end = request_url.find_first_of("/?", start);
	auto host_port = request_url.substr(start, end == string::npos ? string::npos : end - start);
	string host;
	int port = 0;
	if (!webhdfs::SplitHostPort(host_port, host, port)) {
		return host_port;
	}
	if (!host.empty() && host[0] == '[') {
		host = host.substr(1, host.size() - 2);
	}
	return host;
}

vector<std::pair<string, string>> WebHdfsClient::SpnegoHeaders(const string &request_url) {
	string token;
	string error;
	if (!webhdfs::CreateSpnegoToken(config.kerberos_service, HostOf(request_url), token, error)) {
		throw PermissionException("WebHDFS: %s", error);
	}
	return {{"Authorization", "Negotiate " + token}};
}

string WebHdfsClient::FetchKerberosDelegationToken() {
	auto &namenodes = config.namenodes;
	auto start = endpoint.PreferredIndex(namenodes);
	string failures;
	for (idx_t i = 0; i < namenodes.size(); i++) {
		auto &nn = namenodes[(start + i) % namenodes.size()];
		vector<std::pair<string, string>> query;
		if (!config.proxy_user.empty()) {
			query.emplace_back("doas", config.proxy_user);
		}
		auto request_url =
		    webhdfs::BuildRestUrl(config.NameNodeBaseUrl(nn), config.gateway_path, "/", "GETDELEGATIONTOKEN", query);
		auto result =
		    Send(RequestType::GET_REQUEST, request_url, SpnegoHeaders(request_url), nullptr, 0, WebHdfsBodySink());
		endpoint.RecordRequest(nn);
		if (result.status == 0) {
			endpoint.MarkFailed(nn, "unreachable", result.request_error);
			failures += "\n  " + nn + ": " + result.request_error;
			continue;
		}
		RemoteException exception;
		auto kind = ClassifyWebHdfsError(result, exception);
		if (kind == WebHdfsErrorKind::STANDBY) {
			endpoint.MarkFailed(nn, "standby", exception.message);
			failures += "\n  " + nn + ": standby";
			continue;
		}
		if (kind == WebHdfsErrorKind::UNSUPPORTED || result.status == 400 || result.status == 404) {
			// a gateway that authenticates every request but doesn't hand out tokens
			endpoint.spnego_every_request = true;
			endpoint.MarkActive(nn);
			return "";
		}
		if (!result.Ok()) {
			ThrowError("GETDELEGATIONTOKEN", "/", result);
		}
		string token;
		string error;
		if (!webhdfs::ParseToken(result.body, token, error)) {
			// e.g. a NameNode with security disabled answers {"Token":null}: authenticate every request instead
			endpoint.spnego_every_request = true;
			endpoint.MarkActive(nn);
			return "";
		}
		endpoint.MarkActive(nn);
		endpoint.SetKerberosToken(token);
		return token;
	}
	throw IOException("WebHDFS: could not reach any NameNode of \"%s\" to authenticate:%s", url.Prefix(), failures);
}

string WebHdfsClient::KerberosDelegationToken() {
	if (endpoint.spnego_every_request) {
		return "";
	}
	auto token = endpoint.GetKerberosToken();
	if (!token.empty()) {
		return token;
	}
	return FetchKerberosDelegationToken();
}

WebHdfsHttpResult WebHdfsClient::NameNodeCall(RequestType type, const string &op, const string &path,
                                              const vector<std::pair<string, string>> &query,
                                              const WebHdfsBodySink &sink) {
	auto &namenodes = config.namenodes;
	auto start = endpoint.PreferredIndex(namenodes);
	string failures;
	bool token_refreshed = false;
	for (idx_t i = 0; i < namenodes.size(); i++) {
		auto &nn = namenodes[(start + i) % namenodes.size()];
		idx_t retriable_attempts = 0;
		while (true) {
			string kerberos_token;
			vector<std::pair<string, string>> headers;
			if (config.auth == WebHdfsAuthMode::KERBEROS) {
				kerberos_token = KerberosDelegationToken();
			}
			auto all_params = query;
			for (auto &auth : config.AuthParams(kerberos_token)) {
				all_params.push_back(auth);
			}
			auto request_url =
			    webhdfs::BuildRestUrl(config.NameNodeBaseUrl(nn), config.gateway_path, path, op, all_params);
			if (config.auth == WebHdfsAuthMode::KERBEROS && kerberos_token.empty()) {
				headers = SpnegoHeaders(request_url);
			}
			auto result = Send(type, request_url, headers, nullptr, 0, sink);
			endpoint.RecordRequest(nn);
			if (result.status == 0) {
				endpoint.MarkFailed(nn, "unreachable", result.request_error);
				failures += "\n  " + nn + ": " + result.request_error;
				break;
			}
			RemoteException exception;
			auto kind = ClassifyWebHdfsError(result, exception);
			if (kind == WebHdfsErrorKind::STANDBY) {
				endpoint.MarkFailed(nn, "standby", exception.message);
				failures += "\n  " + nn + ": standby (" + exception.exception + ")";
				break;
			}
			if (kind == WebHdfsErrorKind::RETRIABLE && retriable_attempts < 3) {
				retriable_attempts++;
				std::this_thread::sleep_for(std::chrono::milliseconds(200 << retriable_attempts));
				continue;
			}
			if (kind == WebHdfsErrorKind::INVALID_TOKEN && config.auth == WebHdfsAuthMode::KERBEROS &&
			    !token_refreshed) {
				token_refreshed = true;
				endpoint.ClearKerberosToken();
				continue;
			}
			endpoint.MarkActive(nn);
			return result;
		}
	}
	throw IOException("WebHDFS %s on \"%s\": no NameNode could serve the request:%s", op, UrlFor(path), failures);
}

WebHdfsHttpResult WebHdfsClient::DataNodeCall(RequestType type, const string &request_url, const_data_ptr_t data,
                                              idx_t len, const WebHdfsBodySink &sink) {
	vector<std::pair<string, string>> headers;
	if (config.auth == WebHdfsAuthMode::KERBEROS && endpoint.spnego_every_request &&
	    request_url.find("delegation=") == string::npos) {
		headers = SpnegoHeaders(request_url);
	}
	endpoint.RecordDataNodeRequest();
	return Send(type, request_url, headers, data, len, sink);
}

bool WebHdfsClient::TryGetFileStatus(const string &path, FileStatus &out) {
	auto result = NameNodeCall(RequestType::GET_REQUEST, "GETFILESTATUS", path, {});
	if (!result.Ok()) {
		RemoteException exception;
		if (ClassifyWebHdfsError(result, exception) == WebHdfsErrorKind::NOT_FOUND) {
			return false;
		}
		ThrowError("GETFILESTATUS", path, result);
	}
	string error;
	if (!webhdfs::ParseFileStatus(result.body, out, error)) {
		throw IOException("WebHDFS GETFILESTATUS on \"%s\": %s", UrlFor(path), error);
	}
	return true;
}

FileStatus WebHdfsClient::GetFileStatus(const string &path) {
	FileStatus status;
	if (!TryGetFileStatus(path, status)) {
		throw IOException("WebHDFS: no such file or directory: \"%s\"", UrlFor(path));
	}
	return status;
}

void WebHdfsClient::ListStatus(const string &path, const std::function<void(vector<FileStatus> &)> &callback) {
	if (endpoint.batch_listing.load() != 0) {
		string start_after;
		bool first = true;
		while (true) {
			vector<std::pair<string, string>> query;
			if (!start_after.empty()) {
				query.emplace_back("startAfter", start_after);
			}
			auto result = NameNodeCall(RequestType::GET_REQUEST, "LISTSTATUS_BATCH", path, query);
			if (!result.Ok()) {
				RemoteException exception;
				auto kind = ClassifyWebHdfsError(result, exception);
				if (first && (kind == WebHdfsErrorKind::UNSUPPORTED || result.status == 400)) {
					endpoint.batch_listing = 0;
					break;
				}
				ThrowError("LISTSTATUS_BATCH", path, result);
			}
			endpoint.batch_listing = 1;
			vector<FileStatus> entries;
			int64_t remaining = 0;
			string error;
			if (!webhdfs::ParseFileStatusList(result.body, entries, remaining, error)) {
				throw IOException("WebHDFS LISTSTATUS_BATCH on \"%s\": %s", UrlFor(path), error);
			}
			first = false;
			if (!entries.empty()) {
				start_after = entries.back().path_suffix;
			}
			callback(entries);
			if (remaining <= 0 || start_after.empty()) {
				return;
			}
		}
	}
	auto result = NameNodeCall(RequestType::GET_REQUEST, "LISTSTATUS", path, {});
	if (!result.Ok()) {
		ThrowError("LISTSTATUS", path, result);
	}
	vector<FileStatus> entries;
	int64_t remaining = 0;
	string error;
	if (!webhdfs::ParseFileStatusList(result.body, entries, remaining, error)) {
		throw IOException("WebHDFS LISTSTATUS on \"%s\": %s", UrlFor(path), error);
	}
	callback(entries);
}

bool WebHdfsClient::CheckBoolean(const string &op, const string &path, const WebHdfsHttpResult &result) {
	if (!result.Ok()) {
		ThrowError(op, path, result);
	}
	bool value = false;
	string error;
	if (!webhdfs::ParseBooleanResult(result.body, value, error)) {
		throw IOException("WebHDFS %s on \"%s\": %s", op, UrlFor(path), error);
	}
	return value;
}

bool WebHdfsClient::Mkdirs(const string &path, const string &permission) {
	vector<std::pair<string, string>> query;
	if (!permission.empty()) {
		query.emplace_back("permission", permission);
	}
	return CheckBoolean("MKDIRS", path, NameNodeCall(RequestType::PUT_REQUEST, "MKDIRS", path, query));
}

bool WebHdfsClient::Delete(const string &path, bool recursive) {
	return CheckBoolean(
	    "DELETE", path,
	    NameNodeCall(RequestType::DELETE_REQUEST, "DELETE", path, {{"recursive", recursive ? "true" : "false"}}));
}

bool WebHdfsClient::Rename(const string &source, const string &destination) {
	return CheckBoolean("RENAME", source,
	                    NameNodeCall(RequestType::PUT_REQUEST, "RENAME", source, {{"destination", destination}}));
}

string WebHdfsClient::GetJson(const string &op, const string &path, const vector<std::pair<string, string>> &query) {
	auto result = NameNodeCall(RequestType::GET_REQUEST, op, path, query);
	if (!result.Ok()) {
		ThrowError(op, path, result);
	}
	return result.body;
}

string WebHdfsClient::GetJsonViaDataNode(const string &op, const string &path,
                                         const vector<std::pair<string, string>> &query) {
	auto result = NameNodeCall(RequestType::GET_REQUEST, op, path, query);
	if (result.IsRedirect()) {
		if (result.location.empty()) {
			throw IOException("WebHDFS %s on \"%s\": redirect without a Location header", op, UrlFor(path));
		}
		auto data_result = DataNodeCall(RequestType::GET_REQUEST, config.MapDataNodeUrl(result.location), nullptr, 0);
		if (!data_result.Ok()) {
			ThrowError(op + " (DataNode)", path, data_result);
		}
		return data_result.body;
	}
	if (!result.Ok()) {
		ThrowError(op, path, result);
	}
	return result.body;
}

string WebHdfsClient::IdentityKey() const {
	string key = AuthModeToString(config.auth) + "\n" + config.proxy_user + "\n";
	if (config.auth == WebHdfsAuthMode::SIMPLE) {
		key += config.user_name;
	} else if (config.auth == WebHdfsAuthMode::TOKEN) {
		key += config.delegation_token;
	}
	return key;
}

void WebHdfsClient::ReadRange(const string &path, idx_t offset, idx_t length, data_ptr_t buffer,
                              const string &version) {
	idx_t done = 0;
	idx_t request_start = 0;
	WebHdfsBodySink sink;
	sink.reset = [&]() {
		done = request_start;
	};
	sink.write = [&](const_data_ptr_t data, idx_t len) {
		// ignore anything beyond the requested range (stopping the transfer early counts as a failure in some
		// HTTP backends)
		auto take = MinValue<idx_t>(len, length - done);
		memcpy(buffer + done, data, take);
		done += take;
		return true;
	};
	idx_t stalled = 0;
	bool use_cache = !version.empty() && settings.reuse_redirects;
	string cache_key = use_cache ? IdentityKey() + "\n" + path + "\n" + version : string();
	while (done < length) {
		request_start = done;
		auto range_params = vector<std::pair<string, string>> {{"offset", std::to_string(offset + done)},
		                                                       {"length", std::to_string(length - done)}};
		bool served = false;
		if (use_cache) {
			auto cached = endpoint.GetRedirect(cache_key);
			if (!cached.empty()) {
				auto result = DataNodeCall(RequestType::GET_REQUEST, webhdfs::ReplaceQueryParams(cached, range_params),
				                           nullptr, 0, sink);
				if (result.Ok()) {
					served = true;
				} else {
					// stale redirect (token expired, DataNode gone): go through the NameNode again
					done = request_start;
					endpoint.DropRedirect(cache_key, cached);
				}
			}
		}
		if (!served) {
			auto result = NameNodeCall(RequestType::GET_REQUEST, "OPEN", path, range_params, sink);
			if (result.IsRedirect()) {
				if (result.location.empty()) {
					throw IOException("WebHDFS OPEN on \"%s\": redirect without a Location header", UrlFor(path));
				}
				auto location = config.MapDataNodeUrl(result.location);
				done = request_start;
				auto data_result = DataNodeCall(RequestType::GET_REQUEST, location, nullptr, 0, sink);
				if (!data_result.Ok()) {
					ThrowError("OPEN (DataNode)", path, data_result);
				}
				if (use_cache) {
					endpoint.PutRedirect(cache_key, location);
				}
			} else if (!result.Ok()) {
				ThrowError("OPEN", path, result);
			}
		}
		if (done == request_start) {
			if (++stalled >= 3) {
				throw IOException("WebHDFS OPEN on \"%s\": the server returned no data at offset %llu (expected %llu "
				                  "more bytes)",
				                  UrlFor(path), offset + done, length - done);
			}
		} else {
			stalled = 0;
		}
	}
}

void WebHdfsClient::Create(const string &path, const_data_ptr_t data, idx_t len, bool overwrite) {
	vector<std::pair<string, string>> query {{"overwrite", overwrite ? "true" : "false"}};
	if (!config.create_permission.empty()) {
		query.emplace_back("permission", config.create_permission);
	}
	if (config.create_replication > 0) {
		query.emplace_back("replication", std::to_string(config.create_replication));
	}
	if (config.create_block_size > 0) {
		query.emplace_back("blocksize", std::to_string(config.create_block_size));
	}
	auto result = NameNodeCall(RequestType::PUT_REQUEST, "CREATE", path, query);
	if (result.IsRedirect()) {
		if (result.location.empty()) {
			throw IOException("WebHDFS CREATE on \"%s\": redirect without a Location header", UrlFor(path));
		}
		auto data_result = DataNodeCall(RequestType::PUT_REQUEST, config.MapDataNodeUrl(result.location), data, len);
		if (!data_result.Ok()) {
			ThrowError("CREATE (DataNode)", path, data_result);
		}
		return;
	}
	if (!result.Ok()) {
		ThrowError("CREATE", path, result);
	}
	// the server created the file without asking for data: send the data separately
	if (len > 0) {
		Append(path, data, len);
	}
}

void WebHdfsClient::Append(const string &path, const_data_ptr_t data, idx_t len) {
	if (len == 0) {
		return;
	}
	auto result = NameNodeCall(RequestType::POST_REQUEST, "APPEND", path, {});
	if (!result.IsRedirect()) {
		if (result.Ok()) {
			throw IOException("WebHDFS APPEND on \"%s\": the server did not redirect to a DataNode", UrlFor(path));
		}
		ThrowError("APPEND", path, result);
	}
	if (result.location.empty()) {
		throw IOException("WebHDFS APPEND on \"%s\": redirect without a Location header", UrlFor(path));
	}
	auto data_result = DataNodeCall(RequestType::POST_REQUEST, config.MapDataNodeUrl(result.location), data, len);
	if (!data_result.Ok()) {
		ThrowError("APPEND (DataNode)", path, data_result);
	}
}

} // namespace duckdb

#include "webhdfs_http.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

static bool HasHttpProvider(DatabaseInstance &db) {
	return HTTPUtil::Get(db).GetName() != "none";
}

void WebHdfsHttp::TryLoadHttpfs(DatabaseInstance &db) noexcept {
	try {
		if (HasHttpProvider(db)) {
			return;
		}
		ExtensionHelper::TryAutoLoadExtension(db, "httpfs");
	} catch (...) { // NOLINT: loading httpfs here is best-effort; GetHttpUtil reports the failure
	}
}

HTTPUtil &WebHdfsHttp::GetHttpUtil(DatabaseInstance &db) {
	if (!HasHttpProvider(db)) {
		ExtensionHelper::TryAutoLoadExtension(db, "httpfs");
	}
	auto &util = HTTPUtil::Get(db);
	if (util.GetName() == "none") {
		throw MissingExtensionException(
		    "webhdfs requires the httpfs extension for HTTP requests: INSTALL httpfs; LOAD httpfs;");
	}
	return util;
}

unique_ptr<HTTPParams> WebHdfsHttp::CreateParams(DatabaseInstance &db, optional_ptr<FileOpener> opener,
                                                 const string &original_url) {
	auto &util = GetHttpUtil(db);
	unique_ptr<HTTPParams> params;
	if (opener) {
		FileOpenerInfo info;
		info.file_path = original_url;
		params = util.InitializeParameters(opener, &info);
	} else {
		params = util.InitializeParameters(db, original_url);
	}
	// we follow redirects ourselves: DataNode hosts may need rewriting and bodies must not go to the NameNode
	params->follow_location = false;
	params->access_path = original_url;
	return params;
}

static HTTPHeaders MakeHeaders(DatabaseInstance &db, const vector<std::pair<string, string>> &headers) {
	HTTPHeaders result(db);
	for (auto &header : headers) {
		result.Insert(header.first, header.second);
	}
	return result;
}

static void FillResult(WebHdfsHttpResult &result, HTTPResponse &response) {
	if (response.status == HTTPStatusCode::INVALID || response.HasRequestError()) {
		result.status = 0;
		result.request_error = response.GetError();
		if (result.request_error.empty()) {
			result.request_error = "no response";
		}
		return;
	}
	result.status = static_cast<int>(response.status);
	if (response.HasHeader("Location")) {
		result.location = response.GetHeaderValue("Location");
	}
	if (response.HasHeader("WWW-Authenticate")) {
		result.www_authenticate = response.GetHeaderValue("WWW-Authenticate");
	}
}

WebHdfsHttpResult WebHdfsHttp::Get(DatabaseInstance &db, HTTPParams &params, const string &url,
                                   const vector<std::pair<string, string>> &headers, const WebHdfsBodySink &sink) {
	auto &util = GetHttpUtil(db);
	WebHdfsHttpResult result;
	int attempt_status = 0;
	string buffered;
	auto response_handler = [&](const HTTPResponse &response) {
		// called once per attempt, before the body of a 2xx response (and for other statuses too with some
		// backends): reset what a failed earlier attempt delivered
		attempt_status = static_cast<int>(response.status);
		buffered.clear();
		result.bytes_streamed = 0;
		if (sink.reset && attempt_status >= 200 && attempt_status < 300) {
			sink.reset();
		}
		return true;
	};
	auto content_handler = [&](const_data_ptr_t data, idx_t len) {
		if (attempt_status != 0 && (attempt_status < 200 || attempt_status >= 300)) {
			// redirect and error bodies: error bodies are kept in the response by the backend
			return true;
		}
		result.bytes_streamed += len;
		if (sink) {
			return sink.write(data, len);
		}
		buffered.append(const_char_ptr_cast(data), len);
		return true;
	};
	auto http_headers = MakeHeaders(db, headers);
	GetRequestInfo request(url, http_headers, params, response_handler, content_handler);
	request.try_request = true;
	auto response = util.Request(request);
	FillResult(result, *response);
	if (result.Ok()) {
		result.body = std::move(buffered);
	} else {
		result.body = response->body;
	}
	return result;
}

WebHdfsHttpResult WebHdfsHttp::Put(DatabaseInstance &db, HTTPParams &params, const string &url,
                                   const vector<std::pair<string, string>> &headers, const_data_ptr_t data,
                                   idx_t len) {
	auto &util = GetHttpUtil(db);
	auto http_headers = MakeHeaders(db, headers);
	string content_type = "application/octet-stream";
	PutRequestInfo request(url, http_headers, params, data, len, content_type);
	request.try_request = true;
	auto response = util.Request(request);
	WebHdfsHttpResult result;
	FillResult(result, *response);
	result.body = response->body;
	return result;
}

WebHdfsHttpResult WebHdfsHttp::Post(DatabaseInstance &db, HTTPParams &params, const string &url,
                                    const vector<std::pair<string, string>> &headers, const_data_ptr_t data,
                                    idx_t len) {
	auto &util = GetHttpUtil(db);
	auto http_headers = MakeHeaders(db, headers);
	if (!http_headers.HasHeader("Content-Type")) {
		http_headers.Insert("Content-Type", "application/octet-stream");
	}
	PostRequestInfo request(url, http_headers, params, data, len);
	request.try_request = true;
	auto response = util.Request(request);
	WebHdfsHttpResult result;
	FillResult(result, *response);
	result.body = response->body.empty() ? request.buffer_out : response->body;
	return result;
}

WebHdfsHttpResult WebHdfsHttp::Delete(DatabaseInstance &db, HTTPParams &params, const string &url,
                                      const vector<std::pair<string, string>> &headers) {
	auto &util = GetHttpUtil(db);
	auto http_headers = MakeHeaders(db, headers);
	DeleteRequestInfo request(url, http_headers, params);
	request.try_request = true;
	auto response = util.Request(request);
	WebHdfsHttpResult result;
	FillResult(result, *response);
	result.body = response->body;
	return result;
}

} // namespace duckdb

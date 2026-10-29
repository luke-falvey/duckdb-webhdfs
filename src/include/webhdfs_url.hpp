#pragma once

// Parsing of webhdfs:// and swebhdfs:// URLs and building of WebHDFS REST URLs.
// Plain C++ (no DuckDB headers), so it can be unit tested on its own.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace webhdfs {

constexpr const char *WEBHDFS_SCHEME = "webhdfs";
constexpr const char *SWEBHDFS_SCHEME = "swebhdfs";
constexpr int DEFAULT_HTTP_PORT = 9870;
constexpr int DEFAULT_HTTPS_PORT = 9871;

struct WebHdfsUrl {
	//! "webhdfs" or "swebhdfs"
	std::string scheme;
	//! host[:port] exactly as written (the key that selects an endpoint and a secret)
	std::string authority;
	std::string host;
	//! 0 when the URL names no port
	int port = 0;
	//! Absolute, normalized path: starts with '/', no trailing '/' (except the root), no empty segments
	std::string path;

	bool Secure() const {
		return scheme == SWEBHDFS_SCHEME;
	}
	//! scheme://authority
	std::string Prefix() const;
	//! scheme://authority/path
	std::string ToString() const;
};

//! True if the string starts with webhdfs:// or swebhdfs:// (case-insensitive scheme)
bool HasWebHdfsScheme(const std::string &url);

//! Parse a webhdfs:// or swebhdfs:// URL. Returns false and sets `error` when the URL is invalid.
bool ParseWebHdfsUrl(const std::string &url, WebHdfsUrl &out, std::string &error);

//! Normalize an absolute path: collapse repeated '/', drop '.' segments and a trailing '/'.
std::string NormalizePath(const std::string &path);
//! "/a/b" -> "/a", "/a" -> "/", "/" -> "/"
std::string ParentPath(const std::string &path);
//! "/a/b" -> "b", "/" -> ""
std::string BaseName(const std::string &path);
//! Join a directory and a child name: ("/a", "b") -> "/a/b", ("/", "b") -> "/b"
std::string JoinPath(const std::string &dir, const std::string &name);

//! Percent-encode a path for the URL path component: every byte except unreserved characters and '/'.
std::string EncodePath(const std::string &path);
//! Percent-encode a query parameter value: every byte except unreserved characters.
std::string EncodeQueryValue(const std::string &value);
//! Decode %XX escapes (invalid escapes are kept as-is).
std::string PercentDecode(const std::string &value);

//! Split "host:port" (or "[v6]:port"); port is 0 when absent. Returns false on a bad port.
bool SplitHostPort(const std::string &authority, std::string &host, int &port);

//! Builds "<base>/webhdfs/v1<encoded path>?op=<OP>&k=v..."
std::string BuildRestUrl(const std::string &base_url, const std::string &gateway_path, const std::string &path,
                         const std::string &op, const std::vector<std::pair<std::string, std::string>> &params);

//! Replace (or add) query parameters in an existing URL, e.g. to move a DataNode redirect to a new offset.
std::string ReplaceQueryParams(const std::string &url,
                               const std::vector<std::pair<std::string, std::string>> &replacements);

//! Remove credential-bearing query parameters (delegation, user.name, doas, token) for error messages.
std::string RedactUrl(const std::string &url);

//! Split a URL into "scheme://host:port" and the rest. Returns false if it has no scheme.
bool SplitUrlHost(const std::string &url, std::string &scheme_host_port, std::string &rest);

} // namespace webhdfs

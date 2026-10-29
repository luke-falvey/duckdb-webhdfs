#include "webhdfs_url.hpp"

#include <cctype>

namespace webhdfs {

static std::string ToLower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string WebHdfsUrl::Prefix() const {
	return scheme + "://" + authority;
}

std::string WebHdfsUrl::ToString() const {
	return Prefix() + path;
}

static bool StartsWithCI(const std::string &s, const std::string &prefix) {
	if (s.size() < prefix.size()) {
		return false;
	}
	for (size_t i = 0; i < prefix.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
			return false;
		}
	}
	return true;
}

bool HasWebHdfsScheme(const std::string &url) {
	return StartsWithCI(url, "webhdfs://") || StartsWithCI(url, "swebhdfs://");
}

bool SplitHostPort(const std::string &authority, std::string &host, int &port) {
	port = 0;
	std::string port_str;
	if (!authority.empty() && authority[0] == '[') {
		auto close = authority.find(']');
		if (close == std::string::npos) {
			return false;
		}
		host = authority.substr(0, close + 1);
		if (close + 1 < authority.size()) {
			if (authority[close + 1] != ':') {
				return false;
			}
			port_str = authority.substr(close + 2);
		}
	} else {
		auto colon = authority.rfind(':');
		if (colon == std::string::npos) {
			host = authority;
		} else {
			host = authority.substr(0, colon);
			port_str = authority.substr(colon + 1);
		}
	}
	if (host.empty()) {
		return false;
	}
	if (authority.find(':') != std::string::npos && port_str.empty() && authority[0] != '[') {
		return false;
	}
	if (!port_str.empty()) {
		if (port_str.size() > 5) {
			return false;
		}
		int value = 0;
		for (auto c : port_str) {
			if (!std::isdigit(static_cast<unsigned char>(c))) {
				return false;
			}
			value = value * 10 + (c - '0');
		}
		if (value <= 0 || value > 65535) {
			return false;
		}
		port = value;
	}
	return true;
}

bool ParseWebHdfsUrl(const std::string &url, WebHdfsUrl &out, std::string &error) {
	auto sep = url.find("://");
	if (sep == std::string::npos) {
		error = "missing scheme";
		return false;
	}
	auto scheme = ToLower(url.substr(0, sep));
	if (scheme != WEBHDFS_SCHEME && scheme != SWEBHDFS_SCHEME) {
		error = "unsupported scheme '" + scheme + "' (expected webhdfs:// or swebhdfs://)";
		return false;
	}
	auto rest = url.substr(sep + 3);
	auto slash = rest.find('/');
	auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
	auto path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
	if (authority.empty()) {
		error = "missing NameNode host (expected " + scheme + "://host[:port]/path)";
		return false;
	}
	if (authority.find('@') != std::string::npos) {
		error = "user names in the URL are not supported; use a webhdfs secret (USER_NAME)";
		return false;
	}
	std::string host;
	int port = 0;
	if (!SplitHostPort(authority, host, port)) {
		error = "invalid host or port '" + authority + "'";
		return false;
	}
	out.scheme = scheme;
	out.authority = ToLower(authority);
	out.host = ToLower(host);
	out.port = port;
	out.path = NormalizePath(path);
	return true;
}

std::string NormalizePath(const std::string &path) {
	std::vector<std::string> segments;
	size_t pos = 0;
	while (pos <= path.size()) {
		auto next = path.find('/', pos);
		if (next == std::string::npos) {
			next = path.size();
		}
		auto segment = path.substr(pos, next - pos);
		if (!segment.empty() && segment != ".") {
			segments.push_back(std::move(segment));
		}
		pos = next + 1;
	}
	std::string result;
	for (auto &segment : segments) {
		result += "/";
		result += segment;
	}
	return result.empty() ? "/" : result;
}

std::string ParentPath(const std::string &path) {
	auto pos = path.rfind('/');
	if (pos == std::string::npos || pos == 0) {
		return "/";
	}
	return path.substr(0, pos);
}

std::string BaseName(const std::string &path) {
	auto pos = path.rfind('/');
	if (pos == std::string::npos) {
		return path;
	}
	return path.substr(pos + 1);
}

std::string JoinPath(const std::string &dir, const std::string &name) {
	if (dir.empty() || dir == "/") {
		return "/" + name;
	}
	if (dir.back() == '/') {
		return dir + name;
	}
	return dir + "/" + name;
}

static bool IsUnreserved(unsigned char c) {
	return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static void AppendEscaped(std::string &out, unsigned char c) {
	static const char *HEX = "0123456789ABCDEF";
	out += '%';
	out += HEX[c >> 4];
	out += HEX[c & 15];
}

std::string EncodePath(const std::string &path) {
	std::string out;
	out.reserve(path.size());
	for (unsigned char c : path) {
		if (IsUnreserved(c) || c == '/') {
			out += static_cast<char>(c);
		} else {
			AppendEscaped(out, c);
		}
	}
	return out;
}

std::string EncodeQueryValue(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (unsigned char c : value) {
		if (IsUnreserved(c)) {
			out += static_cast<char>(c);
		} else {
			AppendEscaped(out, c);
		}
	}
	return out;
}

static int HexValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

std::string PercentDecode(const std::string &value) {
	std::string out;
	out.reserve(value.size());
	for (size_t i = 0; i < value.size(); i++) {
		if (value[i] == '%' && i + 2 < value.size()) {
			auto hi = HexValue(value[i + 1]);
			auto lo = HexValue(value[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += static_cast<char>(hi * 16 + lo);
				i += 2;
				continue;
			}
		}
		out += value[i];
	}
	return out;
}

std::string BuildRestUrl(const std::string &base_url, const std::string &gateway_path, const std::string &path,
                         const std::string &op, const std::vector<std::pair<std::string, std::string>> &params) {
	std::string url = base_url;
	if (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	if (!gateway_path.empty()) {
		auto prefix = NormalizePath(gateway_path);
		if (prefix != "/") {
			url += EncodePath(prefix);
		}
	}
	url += "/webhdfs/v1";
	url += EncodePath(path);
	url += "?op=";
	url += op;
	for (auto &param : params) {
		url += "&";
		url += param.first;
		url += "=";
		url += EncodeQueryValue(param.second);
	}
	return url;
}

std::string ReplaceQueryParams(const std::string &url,
                               const std::vector<std::pair<std::string, std::string>> &replacements) {
	auto question = url.find('?');
	std::string base = question == std::string::npos ? url : url.substr(0, question);
	std::string query = question == std::string::npos ? std::string() : url.substr(question + 1);
	std::vector<std::string> parts;
	size_t pos = 0;
	while (!query.empty() && pos <= query.size()) {
		auto amp = query.find('&', pos);
		if (amp == std::string::npos) {
			amp = query.size();
		}
		auto part = query.substr(pos, amp - pos);
		if (!part.empty()) {
			auto eq = part.find('=');
			auto key = eq == std::string::npos ? part : part.substr(0, eq);
			bool replaced = false;
			for (auto &r : replacements) {
				if (ToLower(r.first) == ToLower(key)) {
					replaced = true;
					break;
				}
			}
			if (!replaced) {
				parts.push_back(part);
			}
		}
		pos = amp + 1;
	}
	for (auto &r : replacements) {
		parts.push_back(r.first + "=" + EncodeQueryValue(r.second));
	}
	std::string result = base;
	for (size_t i = 0; i < parts.size(); i++) {
		result += i == 0 ? "?" : "&";
		result += parts[i];
	}
	return result;
}

std::string RedactUrl(const std::string &url) {
	auto question = url.find('?');
	if (question == std::string::npos) {
		return url;
	}
	std::string result = url.substr(0, question + 1);
	auto query = url.substr(question + 1);
	size_t pos = 0;
	bool first = true;
	while (pos <= query.size()) {
		auto amp = query.find('&', pos);
		if (amp == std::string::npos) {
			amp = query.size();
		}
		auto part = query.substr(pos, amp - pos);
		if (!part.empty()) {
			auto eq = part.find('=');
			auto key = ToLower(eq == std::string::npos ? part : part.substr(0, eq));
			if (!first) {
				result += "&";
			}
			first = false;
			if (key == "delegation" || key == "user.name" || key == "doas" || key == "token") {
				result += part.substr(0, eq == std::string::npos ? part.size() : eq) + "=REDACTED";
			} else {
				result += part;
			}
		}
		pos = amp + 1;
	}
	return result;
}

bool SplitUrlHost(const std::string &url, std::string &scheme_host_port, std::string &rest) {
	auto sep = url.find("://");
	if (sep == std::string::npos) {
		return false;
	}
	auto slash = url.find('/', sep + 3);
	if (slash == std::string::npos) {
		scheme_host_port = url;
		rest = "/";
	} else {
		scheme_host_port = url.substr(0, slash);
		rest = url.substr(slash);
	}
	return true;
}

} // namespace webhdfs

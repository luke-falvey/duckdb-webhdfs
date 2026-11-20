#include "webhdfs_secret.hpp"

#include "webhdfs_hadoop_config.hpp"
#include "webhdfs_url.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

static bool EqualsIgnoreCase(const string &a, const string &b, idx_t len) {
	for (idx_t i = 0; i < len; i++) {
		if (StringUtil::CharacterToLower(a[i]) != StringUtil::CharacterToLower(b[i])) {
			return false;
		}
	}
	return true;
}

int64_t WebHdfsSecret::ScopeMatchScore(const string &path, const string &scope) {
	constexpr auto NO_MATCH = NumericLimits<int64_t>::Minimum();
	if (scope.empty()) {
		return 0;
	}
	auto score = NumericCast<int64_t>(scope.size());
	auto sep = scope.find("://");
	if (sep == string::npos) {
		return StringUtil::StartsWith(path, scope) ? score : NO_MATCH;
	}
	// scheme and authority: case-insensitive, and the scope's authority must be the path's whole authority, so
	// 'webhdfs://prod' matches neither 'webhdfs://prod2/x' nor 'webhdfs://prod:9870/x'
	auto authority_end = scope.find_first_of("/?#", sep + 3);
	auto head = authority_end == string::npos ? scope.size() : authority_end;
	if (path.size() < head || !EqualsIgnoreCase(path, scope, head)) {
		return NO_MATCH;
	}
	bool scheme_only = head == sep + 3;
	if (!scheme_only && path.size() > head && path[head] != '/' && path[head] != '?' && path[head] != '#') {
		return NO_MATCH;
	}
	// the path part of a scope is a plain prefix, as for other secret types
	if (path.compare(head, scope.size() - head, scope, head, scope.size() - head) != 0) {
		return NO_MATCH;
	}
	return score;
}

int64_t WebHdfsSecret::SecretMatchScore(const BaseSecret &secret, const string &path) {
	auto &scopes = secret.GetScope();
	if (scopes.empty()) {
		return 0;
	}
	auto best = NumericLimits<int64_t>::Minimum();
	for (auto &scope : scopes) {
		best = MaxValue(best, ScopeMatchScore(path, scope));
	}
	return best;
}

int64_t WebHdfsSecret::MatchScore(const string &path) const {
	return SecretMatchScore(*this, path);
}

static const char *const STRING_OPTIONS[] = {"user_name",    "proxy_user",       "delegation_token", "auth",
                                             "gateway_path", "kerberos_service", "permission"};

//! NAMENODES entries are host:port (an entry may hold a comma-separated list): no scheme, no path, a numeric port
static void ValidateNameNodes(const Value &namenodes) {
	for (auto &entry : ListValue::GetChildren(namenodes)) {
		if (entry.IsNull()) {
			continue;
		}
		auto text = entry.ToString();
		auto addresses = webhdfs::SplitCommaList(text);
		if (addresses.empty()) {
			throw InvalidInputException("webhdfs secret: NAMENODES entry '%s' is empty; expected host:port", text);
		}
		for (auto &address : addresses) {
			string host;
			int port = 0;
			bool valid = address.find_first_of("/?#@ \t") == string::npos &&
			             webhdfs::SplitHostPort(address, host, port) && port != 0 &&
			             (host[0] == '[' || host.find(':') == string::npos);
			if (!valid) {
				throw InvalidInputException(
				    "webhdfs secret: NAMENODES entry '%s' must be host:port with a numeric port (no scheme or path)",
				    address);
			}
		}
	}
}

//! A scope that no webhdfs:// or swebhdfs:// URL can match (e.g. 'hdfs://prod' or 'prod') would leave the secret unused
static void ValidateScope(const string &scope) {
	auto lower = StringUtil::Lower(scope);
	for (auto scheme : {"webhdfs://", "swebhdfs://"}) {
		if (StringUtil::StartsWith(lower, scheme) || StringUtil::StartsWith(scheme, lower)) {
			return;
		}
	}
	throw InvalidInputException("webhdfs secret: SCOPE '%s' must start with 'webhdfs://' or 'swebhdfs://'", scope);
}

static unique_ptr<BaseSecret> CreateWebHdfsSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope = {"webhdfs://", "swebhdfs://"};
	}
	for (auto &entry : scope) {
		ValidateScope(entry);
	}
	auto secret = make_uniq<WebHdfsSecret>(scope, input.type, input.provider, input.name);
	secret->redact_keys = {Identifier("delegation_token")};
	for (auto &option : input.options) {
		auto name = StringUtil::Lower(static_cast<const string &>(option.first));
		auto &value = option.second;
		if (value.IsNull()) {
			continue;
		}
		bool is_string = false;
		for (auto key : STRING_OPTIONS) {
			if (name == key) {
				is_string = true;
			}
		}
		if (is_string) {
			auto text = value.ToString();
			if (name == "auth") {
				text = StringUtil::Lower(text);
				if (text != "simple" && text != "token" && text != "kerberos" && text != "none") {
					throw InvalidInputException(
					    "webhdfs secret: AUTH must be 'simple', 'token', 'kerberos' or 'none', not '%s'", text);
				}
			}
			secret->secret_map[Identifier(name)] = Value(text);
		} else if (name == "namenodes") {
			auto namenodes = value.DefaultCastAs(LogicalType::LIST(LogicalType::VARCHAR));
			ValidateNameNodes(namenodes);
			secret->secret_map[Identifier(name)] = std::move(namenodes);
		} else if (name == "datanode_address_map") {
			secret->secret_map[Identifier(name)] =
			    value.DefaultCastAs(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
		} else if (name == "use_ssl") {
			secret->secret_map[Identifier(name)] = value.DefaultCastAs(LogicalType::BOOLEAN);
		} else if (name == "replication" || name == "block_size") {
			auto number = value.DefaultCastAs(LogicalType::BIGINT);
			auto n = number.GetValue<int64_t>();
			// HDFS stores the replication factor as a Java short
			int64_t max = name == "replication" ? NumericLimits<int16_t>::Maximum() : NumericLimits<int64_t>::Maximum();
			if (n <= 0 || n > max) {
				throw InvalidInputException("webhdfs secret: %s must be between 1 and %lld, not %lld",
				                            StringUtil::Upper(name), max, n);
			}
			secret->secret_map[Identifier(name)] = std::move(number);
		} else {
			// the binder rejects options that are not in named_parameters before this function runs
			throw InternalException("webhdfs secret: option '%s' is registered but not handled", name);
		}
	}

	Value auth_value;
	Value token_value;
	string auth = secret->TryGetValue(Identifier("auth"), auth_value) ? auth_value.ToString() : "";
	bool has_token =
	    secret->TryGetValue(Identifier("delegation_token"), token_value) && !token_value.ToString().empty();
	if (auth == "token" && !has_token) {
		throw InvalidInputException("webhdfs secret: AUTH 'token' needs a DELEGATION_TOKEN");
	}
	if (has_token && !auth.empty() && auth != "token") {
		throw InvalidInputException(
		    "webhdfs secret: DELEGATION_TOKEN can only be used with AUTH 'token' (or without AUTH), not AUTH '%s'",
		    auth);
	}
	return std::move(secret);
}

void RegisterWebHdfsSecretType(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = Identifier("webhdfs");
	// new secrets are written as KEY_VALUE_SECRET (core reloads them as KeyValueSecrets); this reads the ones that
	// earlier versions wrote with a custom serialization type
	secret_type.deserializer = KeyValueSecret::Deserialize<WebHdfsSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "webhdfs";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction function = {"webhdfs", "config", CreateWebHdfsSecretFromConfig};
	for (auto key : STRING_OPTIONS) {
		function.named_parameters[key] = LogicalType::VARCHAR;
	}
	function.named_parameters["namenodes"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["datanode_address_map"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	function.named_parameters["use_ssl"] = LogicalType::BOOLEAN;
	function.named_parameters["replication"] = LogicalType::BIGINT;
	function.named_parameters["block_size"] = LogicalType::BIGINT;
	loader.RegisterFunction(function);
}

} // namespace duckdb

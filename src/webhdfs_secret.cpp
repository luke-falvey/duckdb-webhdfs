#include "webhdfs_secret.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/serializer/serializer.hpp"
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

int64_t WebHdfsSecret::MatchScore(const string &path) const {
	if (prefix_paths.empty()) {
		return 0;
	}
	auto best = NumericLimits<int64_t>::Minimum();
	for (auto &scope : prefix_paths) {
		best = MaxValue(best, ScopeMatchScore(path, scope));
	}
	return best;
}

void WebHdfsSecret::Serialize(Serializer &serializer) const {
	SerializeBaseSecret(serializer);
	serializer.WriteProperty(104, "serialization_type", SecretSerializationType::CUSTOM);
	// the fields KeyValueSecret::Deserialize reads
	vector<Value> map_values;
	for (auto &entry : secret_map) {
		child_list_t<Value> map_struct;
		map_struct.emplace_back("key", Value(entry.first));
		map_struct.emplace_back("value", entry.second);
		map_values.push_back(Value::STRUCT(std::move(map_struct)));
	}
	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::ANY);
	serializer.WriteProperty(201, "secret_map", Value::MAP(ListType::GetChildType(map_type), map_values));
	vector<Value> redact_values;
	for (auto &key : redact_keys) {
		redact_values.emplace_back(key);
	}
	serializer.WriteProperty(202, "redact_keys", Value::LIST(LogicalType::VARCHAR, redact_values));
}

static const char *const STRING_OPTIONS[] = {"user_name",    "proxy_user",       "delegation_token", "auth",
                                             "gateway_path", "kerberos_service", "permission"};

static unique_ptr<BaseSecret> CreateWebHdfsSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope = {"webhdfs://", "swebhdfs://"};
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
			secret->secret_map[Identifier(name)] = value.DefaultCastAs(LogicalType::LIST(LogicalType::VARCHAR));
		} else if (name == "datanode_address_map") {
			secret->secret_map[Identifier(name)] =
			    value.DefaultCastAs(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
		} else if (name == "use_ssl") {
			secret->secret_map[Identifier(name)] = value.DefaultCastAs(LogicalType::BOOLEAN);
		} else if (name == "replication" || name == "block_size") {
			secret->secret_map[Identifier(name)] = value.DefaultCastAs(LogicalType::BIGINT);
		} else {
			throw InvalidInputException("Unknown option for a webhdfs secret: '%s'", name);
		}
	}
	return std::move(secret);
}

void RegisterWebHdfsSecretType(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = Identifier("webhdfs");
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

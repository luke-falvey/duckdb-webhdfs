#include "webhdfs_secret.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

static const char *const STRING_OPTIONS[] = {"user_name",  "proxy_user",       "delegation_token", "auth",
                                              "gateway_path", "kerberos_service", "permission"};

static unique_ptr<BaseSecret> CreateWebHdfsSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope = {"webhdfs://", "swebhdfs://"};
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
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
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
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

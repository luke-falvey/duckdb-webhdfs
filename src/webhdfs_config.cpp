#include "webhdfs_config.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "webhdfs_secret.hpp"

#include <cstdlib>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Settings
//===--------------------------------------------------------------------===//

static bool TryGetSetting(optional_ptr<DatabaseInstance> db, optional_ptr<FileOpener> opener, const char *name,
                          Value &result) {
	if (opener && FileOpener::TryGetCurrentSetting(opener, Identifier(name), result)) {
		return !result.IsNull();
	}
	if (db && db->TryGetCurrentSetting(Identifier(name), result)) {
		return !result.IsNull();
	}
	return false;
}

WebHdfsSettings WebHdfsSettings::Load(optional_ptr<DatabaseInstance> db, optional_ptr<FileOpener> opener) {
	if (!db && opener) {
		db = FileOpener::TryGetDatabase(opener);
	}
	WebHdfsSettings settings;
	Value value;
	if (TryGetSetting(db, opener, LIST_PARALLELISM, value)) {
		settings.list_parallelism = MaxValue<idx_t>(1, value.GetValue<uint64_t>());
	}
	if (TryGetSetting(db, opener, INCLUDE_HIDDEN, value)) {
		settings.include_hidden = value.GetValue<bool>();
	}
	if (TryGetSetting(db, opener, SKIP_PERMISSION_ERRORS, value)) {
		settings.skip_permission_errors = value.GetValue<bool>();
	}
	if (TryGetSetting(db, opener, READ_BUFFER_SIZE, value)) {
		settings.read_buffer_size = value.GetValue<uint64_t>();
	}
	if (TryGetSetting(db, opener, WRITE_CHUNK_SIZE, value)) {
		settings.write_chunk_size = MaxValue<idx_t>(1, value.GetValue<uint64_t>());
	}
	if (TryGetSetting(db, opener, REUSE_REDIRECTS, value)) {
		settings.reuse_redirects = value.GetValue<bool>();
	}
	if (TryGetSetting(db, opener, USER_NAME, value)) {
		settings.user_name = value.ToString();
	}
	return settings;
}

idx_t WebHdfsSettings::LoadSmallFileCacheSize(DatabaseInstance &db) {
	// global-only (see Register): read without an opener
	Value value;
	if (TryGetSetting(db, nullptr, SMALL_FILE_CACHE_SIZE, value)) {
		return value.GetValue<uint64_t>();
	}
	return DEFAULT_SMALL_FILE_CACHE_SIZE;
}

namespace {
//! Applies a new webhdfs_small_file_cache_size to every cache right away. Core calls OnSet before it stores the value,
//! so nothing else may set the size from a value it read from the setting (it could undo this one); extension load
//! seeds it only if this hook has not run yet (WebHdfsFileSystem::SeedSmallFileCacheSize)
struct CacheSizeOptionInfo : public ExtensionOptionInfo {
	explicit CacheSizeOptionInfo(std::function<void(idx_t)> apply_p) : apply(std::move(apply_p)) {
	}
	void OnSet(ClientContext &context, SetScope scope, Value &parameter, bool is_reset) const override {
		if (apply) {
			// NULL reads as the default everywhere else (Load), so it must here too
			apply(parameter.IsNull() ? WebHdfsSettings::DEFAULT_SMALL_FILE_CACHE_SIZE : parameter.GetValue<uint64_t>());
		}
	}
	std::function<void(idx_t)> apply;
};
} // namespace

void WebHdfsSettings::Register(DatabaseInstance &db, std::function<void(idx_t)> on_cache_size) {
	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption(LIST_PARALLELISM,
	                          "Maximum number of concurrent WebHDFS directory listings in globs and recursive "
	                          "listings; 1 lists one directory at a time",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_LIST_PARALLELISM));
	config.AddExtensionOption(INCLUDE_HIDDEN,
	                          "Return hidden WebHDFS entries (names starting with '_' or '.', e.g. _SUCCESS) from "
	                          "globs and listings; by default they are skipped unless a pattern names them",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption(SKIP_PERMISSION_ERRORS,
	                          "Skip directories that can't be listed for lack of permission during WebHDFS globs and "
	                          "recursive listings, instead of failing the query",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption(READ_BUFFER_SIZE,
	                          "Minimum WebHDFS read size in bytes: small sequential reads fetch this much ahead; 0 "
	                          "disables read-ahead",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_READ_BUFFER_SIZE));
	config.AddExtensionOption(WRITE_CHUNK_SIZE,
	                          "Bytes buffered per WebHDFS file being written before they are sent (one CREATE, then "
	                          "one APPEND per chunk)",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_WRITE_CHUNK_SIZE));
	config.AddExtensionOption(REUSE_REDIRECTS,
	                          "Reuse a file's DataNode redirect for later reads of the same file, skipping the "
	                          "NameNode round trip",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption(USER_NAME,
	                          "User name for WebHDFS simple authentication when no webhdfs secret sets one (default: "
	                          "HADOOP_USER_NAME, else the OS user)",
	                          LogicalType::VARCHAR, Value());
	// database-wide (the cache is shared by every session): a plain SET sets it globally, SET SESSION is an error
	ExtensionOption cache_size("Bytes of small WebHDFS files (read whole) kept in memory per cluster, so a file "
	                           "opened again (e.g. at bind time and to scan it) is not fetched twice; 0 disables the "
	                           "cache. Database-wide",
	                           LogicalType::UBIGINT, nullptr, Value::UBIGINT(DEFAULT_SMALL_FILE_CACHE_SIZE),
	                           SetScope::GLOBAL, false, false);
	cache_size.supports_session = false;
	cache_size.info = make_shared_ptr<CacheSizeOptionInfo>(std::move(on_cache_size));
	config.AddExtensionOption(SMALL_FILE_CACHE_SIZE, std::move(cache_size));
}

//===--------------------------------------------------------------------===//
// Connection configuration
//===--------------------------------------------------------------------===//

string AuthModeToString(WebHdfsAuthMode mode) {
	switch (mode) {
	case WebHdfsAuthMode::SIMPLE:
		return "simple";
	case WebHdfsAuthMode::TOKEN:
		return "token";
	case WebHdfsAuthMode::KERBEROS:
		return "kerberos";
	case WebHdfsAuthMode::NONE:
		return "none";
	}
	return "unknown";
}

string WebHdfsConfig::NameNodeBaseUrl(const string &namenode) const {
	return (use_ssl ? "https://" : "http://") + namenode;
}

string WebHdfsConfig::MapDataNodeUrl(const string &url) const {
	if (datanode_address_map.empty()) {
		return url;
	}
	auto sep = url.find("://");
	if (sep == string::npos) {
		return url;
	}
	auto host_start = sep + 3;
	auto host_end = url.find('/', host_start);
	if (host_end == string::npos) {
		host_end = url.size();
	}
	auto host_port = StringUtil::Lower(url.substr(host_start, host_end - host_start));
	auto entry = datanode_address_map.find(host_port);
	if (entry == datanode_address_map.end()) {
		// also allow mapping a bare host name
		auto colon = host_port.rfind(':');
		if (colon != string::npos) {
			auto host_entry = datanode_address_map.find(host_port.substr(0, colon));
			if (host_entry != datanode_address_map.end()) {
				return url.substr(0, host_start) + host_entry->second + host_port.substr(colon) + url.substr(host_end);
			}
		}
		return url;
	}
	return url.substr(0, host_start) + entry->second + url.substr(host_end);
}

vector<std::pair<string, string>> WebHdfsConfig::AuthParams(const string &kerberos_delegation_token) const {
	vector<std::pair<string, string>> params;
	switch (auth) {
	case WebHdfsAuthMode::SIMPLE:
		if (!user_name.empty()) {
			params.emplace_back("user.name", user_name);
		}
		break;
	case WebHdfsAuthMode::TOKEN:
		params.emplace_back("delegation", delegation_token);
		break;
	case WebHdfsAuthMode::KERBEROS:
		if (!kerberos_delegation_token.empty()) {
			params.emplace_back("delegation", kerberos_delegation_token);
		}
		break;
	case WebHdfsAuthMode::NONE:
		break;
	}
	if (!proxy_user.empty()) {
		params.emplace_back("doas", proxy_user);
	}
	return params;
}

string CurrentOsUser() {
#ifndef _WIN32
	// getpwuid_r: clients are created on many threads at once
	struct passwd pwd;
	struct passwd *found = nullptr;
	vector<char> buffer(16384);
	if (getpwuid_r(geteuid(), &pwd, buffer.data(), buffer.size(), &found) == 0 && found && found->pw_name) {
		return found->pw_name;
	}
	const char *user = std::getenv("USER");
#else
	const char *user = std::getenv("USERNAME");
#endif
	return user ? user : "";
}

static string GetEnv(const char *name) {
	const char *value = std::getenv(name);
	return value ? value : "";
}

static CatalogTransaction SecretTransaction(DatabaseInstance &db, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	return context ? CatalogTransaction::GetSystemCatalogTransaction(*context)
	               : CatalogTransaction::GetSystemTransaction(db);
}

//! Every webhdfs secret visible to this transaction, from every storage (temporary, persistent, ...)
static vector<SecretEntry> AllWebHdfsSecrets(DatabaseInstance &db, optional_ptr<FileOpener> opener) {
	vector<SecretEntry> result;
	for (auto &entry : SecretManager::Get(db).AllSecrets(SecretTransaction(db, opener))) {
		if (entry.secret && StringUtil::CIEquals(entry.secret->GetType().GetIdentifierName(), "webhdfs")) {
			result.push_back(std::move(entry));
		}
	}
	return result;
}

vector<string> WebHdfsSecretAuthorities(DatabaseInstance &db, optional_ptr<FileOpener> opener, const string &scheme) {
	vector<string> result;
	for (auto &entry : AllWebHdfsSecrets(db, opener)) {
		for (auto &scope : entry.secret->GetScope()) {
			auto authority = webhdfs::ScopeAuthority(scope, scheme);
			if (!authority.empty()) {
				result.push_back(std::move(authority));
			}
		}
	}
	return result;
}

//! The tie-break offsets of core's storages (SecretStorage::*_STORAGE_OFFSET): on an equal score, lower wins
static int64_t StorageTieBreakOffset(const string &storage) {
	if (storage == SecretManager::TRANSACTION_STORAGE_NAME) {
		return SecretStorage::TRANSACTION_STORAGE_OFFSET;
	}
	if (storage == SecretManager::CONNECTION_STORAGE_NAME) {
		return SecretStorage::CONNECTION_STORAGE_OFFSET;
	}
	if (storage == SecretManager::TEMPORARY_STORAGE_NAME) {
		return SecretStorage::TEMPORARY_STORAGE_OFFSET;
	}
	if (storage == SecretManager::LOCAL_FILE_STORAGE_NAME) {
		return SecretStorage::LOCAL_FILE_STORAGE_OFFSET;
	}
	return SecretStorage::LOCAL_FILE_STORAGE_OFFSET + 10;
}

//! The webhdfs secret for a path. Scored here with WebHdfsSecret::ScopeMatchScore rather than through
//! SecretManager::LookupSecret: persistent secrets are stored as KEY_VALUE_SECRETs and reload as plain KeyValueSecrets,
//! whose MatchScore is core's string-prefix match ('webhdfs://prod' would match 'webhdfs://prod2/x'). Scoring every
//! webhdfs secret the same way keeps in-memory and persistent secrets alike. Ranking follows core: the longest
//! matching scope, then the storage tie-break offset, then the smallest name.
static unique_ptr<SecretEntry> LookupSecret(DatabaseInstance &db, optional_ptr<FileOpener> opener, const string &path) {
	unique_ptr<SecretEntry> best;
	int64_t best_score = NumericLimits<int64_t>::Minimum();
	for (auto &entry : AllWebHdfsSecrets(db, opener)) {
		auto match = WebHdfsSecret::SecretMatchScore(*entry.secret, path);
		if (match == NumericLimits<int64_t>::Minimum()) {
			continue;
		}
		auto score = 100 * match - StorageTieBreakOffset(entry.storage_mode);
		if (!best || score > best_score || (score == best_score && entry.secret->GetName() < best->secret->GetName())) {
			best = make_uniq<SecretEntry>(entry);
			best_score = score;
		}
	}
	return best;
}

static bool SecretString(optional_ptr<const KeyValueSecret> secret, const char *key, string &out) {
	if (!secret) {
		return false;
	}
	Value value;
	if (!secret->TryGetValue(Identifier(key), value) || value.IsNull()) {
		return false;
	}
	out = value.ToString();
	return true;
}

static void ParseAddressList(const Value &value, vector<string> &out) {
	if (value.type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(value)) {
			if (!child.IsNull()) {
				ParseAddressList(child, out);
			}
		}
		return;
	}
	for (auto &item : webhdfs::SplitCommaList(value.ToString())) {
		out.push_back(StringUtil::Lower(item));
	}
}

WebHdfsConfig ResolveWebHdfsConfig(DatabaseInstance &db, optional_ptr<FileOpener> opener,
                                   const webhdfs::WebHdfsUrl &url, const webhdfs::HadoopConfig &site,
                                   const WebHdfsSettings &settings) {
	WebHdfsConfig config;
	config.scheme = url.scheme;
	config.authority = url.authority;
	config.use_ssl = url.Secure();

	auto secret_entry = LookupSecret(db, opener, url.ToString());
	optional_ptr<const KeyValueSecret> secret =
	    secret_entry ? dynamic_cast<const KeyValueSecret *>(secret_entry->secret.get()) : nullptr;
	if (secret) {
		config.secret_name = secret->GetName().GetIdentifierName();
	}

	Value value;
	if (secret && secret->TryGetValue(Identifier("use_ssl"), value) && !value.IsNull()) {
		config.use_ssl = value.GetValue<bool>();
	}

	// NameNodes: the secret's list, else an HA nameservice from hdfs-site.xml, else the URL's host:port
	if (secret && secret->TryGetValue(Identifier("namenodes"), value) && !value.IsNull()) {
		ParseAddressList(value, config.namenodes);
		config.namenode_source = "secret";
	}
	if (config.namenodes.empty() && url.port == 0) {
		auto addresses = site.NameNodeAddresses(url.host, config.use_ssl);
		for (auto &address : addresses) {
			config.namenodes.push_back(StringUtil::Lower(address));
		}
		if (!config.namenodes.empty()) {
			config.namenode_source = "hdfs-site.xml";
		}
	}
	if (config.namenodes.empty()) {
		auto port =
		    url.port != 0 ? url.port : (config.use_ssl ? webhdfs::DEFAULT_HTTPS_PORT : webhdfs::DEFAULT_HTTP_PORT);
		config.namenodes.push_back(url.host + ":" + std::to_string(port));
		config.namenode_source = "url";
	}

	SecretString(secret, "gateway_path", config.gateway_path);
	SecretString(secret, "kerberos_service", config.kerberos_service);
	SecretString(secret, "permission", config.create_permission);
	if (secret && secret->TryGetValue(Identifier("replication"), value) && !value.IsNull()) {
		config.create_replication = value.GetValue<int64_t>();
	}
	if (secret && secret->TryGetValue(Identifier("block_size"), value) && !value.IsNull()) {
		config.create_block_size = value.GetValue<int64_t>();
	}
	if (secret && secret->TryGetValue(Identifier("datanode_address_map"), value) && !value.IsNull()) {
		for (auto &entry : MapValue::GetChildren(value)) {
			auto &kv = StructValue::GetChildren(entry);
			if (kv.size() == 2 && !kv[0].IsNull() && !kv[1].IsNull()) {
				config.datanode_address_map[StringUtil::Lower(kv[0].ToString())] = kv[1].ToString();
			}
		}
	}

	// identity
	if (!SecretString(secret, "user_name", config.user_name)) {
		config.user_name = settings.user_name;
	}
	if (config.user_name.empty()) {
		config.user_name = GetEnv("HADOOP_USER_NAME");
	}
	if (config.user_name.empty()) {
		config.user_name = CurrentOsUser();
	}
	if (!SecretString(secret, "proxy_user", config.proxy_user)) {
		config.proxy_user = GetEnv("HADOOP_PROXY_USER");
	}
	SecretString(secret, "delegation_token", config.delegation_token);

	string auth;
	if (SecretString(secret, "auth", auth)) {
		auth = StringUtil::Lower(auth);
	} else if (!config.delegation_token.empty()) {
		auth = "token";
	} else if (StringUtil::Lower(site.Get("hadoop.security.authentication")) == "kerberos") {
		auth = "kerberos";
	} else {
		auth = "simple";
	}
	if (auth == "simple") {
		config.auth = WebHdfsAuthMode::SIMPLE;
	} else if (auth == "token") {
		if (config.delegation_token.empty()) {
			throw InvalidConfigurationException("webhdfs secret '%s': AUTH 'token' needs a DELEGATION_TOKEN",
			                                    config.secret_name);
		}
		config.auth = WebHdfsAuthMode::TOKEN;
	} else if (auth == "kerberos") {
		config.auth = WebHdfsAuthMode::KERBEROS;
	} else if (auth == "none") {
		config.auth = WebHdfsAuthMode::NONE;
	} else {
		throw InvalidConfigurationException("Unknown webhdfs AUTH '%s' (expected simple, token, kerberos or none)",
		                                    auth);
	}
	return config;
}

} // namespace duckdb

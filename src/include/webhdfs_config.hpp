#pragma once

// Extension settings, and the per-request connection configuration of a webhdfs:// authority, resolved from the
// matching `webhdfs` secret, the extension settings, Hadoop's client config files and the environment.

#include "duckdb.hpp"
#include "duckdb/common/file_opener.hpp"
#include "webhdfs_hadoop_config.hpp"
#include "webhdfs_url.hpp"

namespace duckdb {

struct WebHdfsSettings {
	static constexpr const char *LIST_PARALLELISM = "webhdfs_list_parallelism";
	static constexpr const char *INCLUDE_HIDDEN = "webhdfs_include_hidden";
	static constexpr const char *SKIP_PERMISSION_ERRORS = "webhdfs_skip_permission_errors";
	static constexpr const char *READ_BUFFER_SIZE = "webhdfs_read_buffer_size";
	static constexpr const char *WRITE_CHUNK_SIZE = "webhdfs_write_chunk_size";
	static constexpr const char *REUSE_REDIRECTS = "webhdfs_reuse_redirects";
	static constexpr const char *USER_NAME = "webhdfs_user_name";
	static constexpr const char *PARTITION_PRUNING = "webhdfs_partition_pruning";

	static constexpr idx_t DEFAULT_LIST_PARALLELISM = 16;
	static constexpr idx_t DEFAULT_READ_BUFFER_SIZE = 1024 * 1024;
	static constexpr idx_t DEFAULT_WRITE_CHUNK_SIZE = 64 * 1024 * 1024;

	idx_t list_parallelism = DEFAULT_LIST_PARALLELISM;
	bool include_hidden = false;
	bool skip_permission_errors = false;
	idx_t read_buffer_size = DEFAULT_READ_BUFFER_SIZE;
	idx_t write_chunk_size = DEFAULT_WRITE_CHUNK_SIZE;
	bool reuse_redirects = true;
	string user_name;
	bool partition_pruning = true;

	//! Current values (session settings through the opener, else global settings of `db`)
	static WebHdfsSettings Load(optional_ptr<DatabaseInstance> db, optional_ptr<FileOpener> opener);
	//! Register the settings on the database
	static void Register(DatabaseInstance &db);
};

enum class WebHdfsAuthMode : uint8_t {
	//! user.name= query parameter (Hadoop "simple" authentication)
	SIMPLE,
	//! delegation= token query parameter
	TOKEN,
	//! SPNEGO with the Kerberos ticket cache; a delegation token is fetched once and used afterwards
	KERBEROS,
	//! No WebHDFS credentials (e.g. a gateway authenticated through an `http` secret)
	NONE
};

string AuthModeToString(WebHdfsAuthMode mode);

struct WebHdfsConfig {
	string scheme;
	string authority;
	//! HTTPS to the NameNodes (swebhdfs://, or USE_SSL in the secret)
	bool use_ssl = false;
	//! NameNode HTTP addresses ("host:port"), in failover order
	vector<string> namenodes;
	//! Where the NameNode list came from: "url", "secret" or "hdfs-site.xml"
	string namenode_source;
	//! Path prefix before /webhdfs/v1 (Apache Knox: /gateway/<topology>)
	string gateway_path;
	WebHdfsAuthMode auth = WebHdfsAuthMode::SIMPLE;
	string user_name;
	string proxy_user;
	string delegation_token;
	//! Kerberos service name of the NameNode HTTP principal (HTTP/<host>@REALM)
	string kerberos_service = "HTTP";
	//! "host:port" -> "host:port" rewrites applied to DataNode redirects
	unordered_map<string, string> datanode_address_map;
	//! Options for files created by writes (empty / -1: NameNode defaults)
	string create_permission;
	int64_t create_replication = -1;
	int64_t create_block_size = -1;
	//! Name of the secret that applied, if any
	string secret_name;

	//! "http(s)://<namenode>"
	string NameNodeBaseUrl(const string &namenode) const;
	//! Rewrite the host:port of a DataNode URL using datanode_address_map
	string MapDataNodeUrl(const string &url) const;
	//! Query parameters carrying the identity (user.name / doas / delegation)
	vector<std::pair<string, string>> AuthParams(const string &kerberos_delegation_token) const;
};

//! Resolve the connection configuration for `url`. `site` holds hdfs-site.xml and core-site.xml.
WebHdfsConfig ResolveWebHdfsConfig(DatabaseInstance &db, optional_ptr<FileOpener> opener, const webhdfs::WebHdfsUrl &url,
                                   const webhdfs::HadoopConfig &site, const WebHdfsSettings &settings);

//! The OS user name (fallback identity for simple authentication)
string CurrentOsUser();

} // namespace duckdb

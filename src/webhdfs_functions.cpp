#include "webhdfs_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "webhdfs_filesystem.hpp"
#include "webhdfs_glob.hpp"
#include "webhdfs_json.hpp"

namespace duckdb {

using webhdfs::FileStatus;
using webhdfs::JsonDocument;
using webhdfs::JsonValue;

using Row = vector<Value>;

static optional_ptr<FileOpener> ContextOpener(ClientContext &context) {
	return ClientData::Get(context).file_opener.get();
}

static Value TimestampValue(int64_t epoch_ms) {
	return Value::TIMESTAMPTZ(timestamp_tz_t(WebHdfsTimestamp(epoch_ms)));
}

static Value OptionalBigint(int64_t value) {
	return value < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(value);
}

static Value OptionalString(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

static Value StringList(JsonValue array) {
	vector<Value> values;
	for (idx_t i = 0; i < array.Size(); i++) {
		values.emplace_back(array.At(i).GetString());
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

static std::unique_ptr<JsonDocument> ParseJson(const string &body, const string &op) {
	string error;
	auto doc = JsonDocument::Parse(body, error);
	if (!doc) {
		throw IOException("WebHDFS %s: %s", op, error);
	}
	return doc;
}

//===--------------------------------------------------------------------===//
// Row sources
//===--------------------------------------------------------------------===//

class RowSource {
public:
	virtual ~RowSource() = default;
	//! Append up to `max` rows; false when exhausted
	virtual bool Next(vector<Row> &rows, idx_t max) = 0;
};

class VectorRowSource : public RowSource {
public:
	explicit VectorRowSource(vector<Row> rows_p) : rows(std::move(rows_p)) {
	}
	bool Next(vector<Row> &out, idx_t max) override {
		idx_t added = 0;
		while (position < rows.size() && added < max) {
			out.push_back(std::move(rows[position++]));
			added++;
		}
		return added > 0;
	}

private:
	vector<Row> rows;
	idx_t position = 0;
};

//===--------------------------------------------------------------------===//
// webhdfs_ls / webhdfs_stat columns
//===--------------------------------------------------------------------===//

static const vector<std::pair<const char *, LogicalType>> &EntryColumns() {
	static const vector<std::pair<const char *, LogicalType>> columns = {
	    {"path", LogicalType::VARCHAR},          {"name", LogicalType::VARCHAR},
	    {"type", LogicalType::VARCHAR},          {"size", LogicalType::BIGINT},
	    {"owner", LogicalType::VARCHAR},         {"group", LogicalType::VARCHAR},
	    {"permissions", LogicalType::VARCHAR},   {"mode", LogicalType::USMALLINT},
	    {"replication", LogicalType::INTEGER},   {"block_size", LogicalType::BIGINT},
	    {"last_modified", LogicalType::TIMESTAMP_TZ}, {"last_accessed", LogicalType::TIMESTAMP_TZ},
	    {"file_id", LogicalType::BIGINT},        {"children_num", LogicalType::BIGINT},
	    {"storage_policy", LogicalType::INTEGER}, {"ec_policy", LogicalType::VARCHAR}};
	return columns;
}

static Row EntryRow(const string &url, const string &path, const FileStatus &status) {
	auto name = webhdfs::BaseName(path);
	string type = status.is_dir ? "directory" : (status.is_symlink ? "symlink" : "file");
	return {Value(url),
	        Value(name),
	        Value(type),
	        Value::BIGINT(status.length),
	        Value(status.owner),
	        Value(status.group),
	        Value(webhdfs::FormatPermission(status.permission)),
	        Value::USMALLINT(status.permission),
	        status.replication < 0 ? Value(LogicalType::INTEGER) : Value::INTEGER(status.replication),
	        OptionalBigint(status.block_size),
	        TimestampValue(status.modification_time),
	        TimestampValue(status.access_time),
	        OptionalBigint(status.file_id),
	        status.is_dir ? OptionalBigint(status.children_num) : Value(LogicalType::BIGINT),
	        status.storage_policy < 0 ? Value(LogicalType::INTEGER) : Value::INTEGER(status.storage_policy),
	        OptionalString(status.ec_policy)};
}

class WalkRowSource : public RowSource {
public:
	WalkRowSource(shared_ptr<WebHdfsClient> client_p, unique_ptr<WebHdfsWalk> walk_p)
	    : client(std::move(client_p)), walk(std::move(walk_p)) {
	}
	bool Next(vector<Row> &rows, idx_t max) override {
		vector<WebHdfsWalkEntry> entries;
		if (!walk->Next(entries, max)) {
			return false;
		}
		for (auto &entry : entries) {
			rows.push_back(EntryRow(client->UrlFor(entry.path), entry.path, entry.status));
		}
		return true;
	}

private:
	shared_ptr<WebHdfsClient> client;
	unique_ptr<WebHdfsWalk> walk;
};

//===--------------------------------------------------------------------===//
// Function definitions
//===--------------------------------------------------------------------===//

struct MetaCall {
	WebHdfsFileSystem &fs;
	ClientContext &context;
	string url;
	const named_argument_map_t &named;

	shared_ptr<WebHdfsClient> Client(webhdfs::WebHdfsUrl &parsed) {
		return fs.CreateClient(url, ContextOpener(context), parsed);
	}
	bool NamedBool(const char *name, bool default_value) const {
		for (auto &entry : named) {
			if (StringUtil::CIEquals(entry.first.GetIdentifierName(), name) && !entry.second.IsNull()) {
				return entry.second.GetValue<bool>();
			}
		}
		return default_value;
	}
	Value Named(const char *name) const {
		for (auto &entry : named) {
			if (StringUtil::CIEquals(entry.first.GetIdentifierName(), name)) {
				return entry.second;
			}
		}
		return Value();
	}
};

typedef unique_ptr<RowSource> (*make_rows_t)(MetaCall &call);

struct MetaFunctionDef {
	const char *name;
	vector<std::pair<const char *, LogicalType>> columns;
	make_rows_t make;
	vector<std::pair<const char *, LogicalType>> keywords;
};

static unique_ptr<RowSource> SingleRow(Row row) {
	vector<Row> rows;
	rows.push_back(std::move(row));
	return make_uniq<VectorRowSource>(std::move(rows));
}

static unique_ptr<RowSource> MakeLs(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto &settings = client->Settings();
	WebHdfsWalkOptions options;
	options.include_hidden = call.NamedBool("include_hidden", settings.include_hidden);
	options.skip_permission_errors = call.NamedBool("skip_permission_errors", settings.skip_permission_errors);
	options.parallelism = settings.list_parallelism;
	options.files_only = false;
	if (!webhdfs::HasGlobCharacters(parsed.path)) {
		auto status = client->GetFileStatus(parsed.path);
		if (!status.is_dir) {
			return SingleRow(EntryRow(client->UrlFor(parsed.path), parsed.path, status));
		}
	}
	auto walk = make_uniq<WebHdfsWalk>(client, parsed.path, options);
	return make_uniq<WalkRowSource>(client, std::move(walk));
}

static unique_ptr<RowSource> MakeStat(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto status = client->GetFileStatus(parsed.path);
	return SingleRow(EntryRow(client->UrlFor(parsed.path), parsed.path, status));
}

static unique_ptr<RowSource> MakeContentSummary(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETCONTENTSUMMARY", parsed.path), "GETCONTENTSUMMARY");
	auto summary = doc->Root()["ContentSummary"];
	auto ec = summary["ecPolicy"];
	return SingleRow({Value(client->UrlFor(parsed.path)), Value::BIGINT(summary["length"].GetInt()),
	                  Value::BIGINT(summary["fileCount"].GetInt()), Value::BIGINT(summary["directoryCount"].GetInt()),
	                  Value::BIGINT(summary["spaceConsumed"].GetInt()), OptionalBigint(summary["quota"].GetInt(-1)),
	                  OptionalBigint(summary["spaceQuota"].GetInt(-1)),
	                  OptionalString(ec.IsString() ? ec.GetString() : "")});
}

static LogicalType TypeQuotaType() {
	child_list_t<LogicalType> fields = {{"consumed", LogicalType::BIGINT}, {"quota", LogicalType::BIGINT}};
	return LogicalType::MAP(LogicalType::VARCHAR, LogicalType::STRUCT(fields));
}

static unique_ptr<RowSource> MakeQuotaUsage(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETQUOTAUSAGE", parsed.path), "GETQUOTAUSAGE");
	auto usage = doc->Root()["QuotaUsage"];
	vector<Value> keys;
	vector<Value> values;
	usage["typeQuota"].ForEachMember([&](const string &type, JsonValue entry) {
		keys.emplace_back(type);
		values.push_back(Value::STRUCT({{"consumed", OptionalBigint(entry["consumed"].GetInt(-1))},
		                                {"quota", OptionalBigint(entry["quota"].GetInt(-1))}}));
	});
	auto struct_type = MapType::ValueType(TypeQuotaType());
	return SingleRow({Value(client->UrlFor(parsed.path)), Value::BIGINT(usage["fileAndDirectoryCount"].GetInt()),
	                  OptionalBigint(usage["quota"].GetInt(-1)), Value::BIGINT(usage["spaceConsumed"].GetInt()),
	                  OptionalBigint(usage["spaceQuota"].GetInt(-1)),
	                  Value::MAP(LogicalType::VARCHAR, struct_type, std::move(keys), std::move(values))});
}

static unique_ptr<RowSource> MakeBlockLocations(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto offset_value = call.Named("offset");
	auto length_value = call.Named("length");
	int64_t offset = offset_value.IsNull() ? 0 : offset_value.GetValue<int64_t>();
	int64_t length = length_value.IsNull() ? NumericLimits<int64_t>::Maximum() : length_value.GetValue<int64_t>();
	vector<std::pair<string, string>> query {{"offset", std::to_string(offset)}, {"length", std::to_string(length)}};
	auto url = client->UrlFor(parsed.path);
	vector<Row> rows;
	string body;
	bool located_blocks = false;
	try {
		body = client->GetJson("GETFILEBLOCKLOCATIONS", parsed.path, query);
	} catch (NotImplementedException &) {
		// NameNodes before Hadoop 3.3 only have the internal GET_BLOCK_LOCATIONS
		body = client->GetJson("GET_BLOCK_LOCATIONS", parsed.path, query);
		located_blocks = true;
	}
	auto doc = ParseJson(body, "block locations");
	if (!located_blocks) {
		auto blocks = doc->Root()["BlockLocations"]["BlockLocation"];
		for (idx_t i = 0; i < blocks.Size(); i++) {
			auto block = blocks.At(i);
			rows.push_back({Value(url), Value::BIGINT(block["offset"].GetInt()), Value::BIGINT(block["length"].GetInt()),
			                StringList(block["hosts"]), StringList(block["names"]),
			                StringList(block["topologyPaths"]), StringList(block["storageTypes"]),
			                Value::BOOLEAN(block["corrupt"].GetBool())});
		}
	} else {
		auto blocks = doc->Root()["LocatedBlocks"]["locatedBlocks"];
		for (idx_t i = 0; i < blocks.Size(); i++) {
			auto block = blocks.At(i);
			vector<Value> hosts, names, topology, storage;
			auto locations = block["locations"];
			for (idx_t l = 0; l < locations.Size(); l++) {
				auto location = locations.At(l);
				auto name = location["ipAddr"].GetString() + ":" + std::to_string(location["xferPort"].GetInt());
				hosts.emplace_back(location["hostName"].GetString());
				names.emplace_back(name);
				topology.emplace_back(location["networkLocation"].GetString() + "/" + name);
				storage.emplace_back(location["storageType"].GetString("DISK"));
			}
			rows.push_back({Value(url), Value::BIGINT(block["startOffset"].GetInt()),
			                Value::BIGINT(block["block"]["numBytes"].GetInt()),
			                Value::LIST(LogicalType::VARCHAR, std::move(hosts)),
			                Value::LIST(LogicalType::VARCHAR, std::move(names)),
			                Value::LIST(LogicalType::VARCHAR, std::move(topology)),
			                Value::LIST(LogicalType::VARCHAR, std::move(storage)),
			                Value::BOOLEAN(block["isCorrupt"].GetBool())});
		}
	}
	return make_uniq<VectorRowSource>(std::move(rows));
}

static unique_ptr<RowSource> MakeChecksum(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJsonViaDataNode("GETFILECHECKSUM", parsed.path), "GETFILECHECKSUM");
	auto checksum = doc->Root()["FileChecksum"];
	return SingleRow({Value(client->UrlFor(parsed.path)), Value(checksum["algorithm"].GetString()),
	                  Value(checksum["bytes"].GetString()), Value::INTEGER(checksum["length"].GetInt())});
}

static unique_ptr<RowSource> MakeAcl(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETACLSTATUS", parsed.path), "GETACLSTATUS");
	auto acl = doc->Root()["AclStatus"];
	auto permission = webhdfs::ParseOctalPermission(acl["permission"].GetString());
	return SingleRow({Value(client->UrlFor(parsed.path)), Value(acl["owner"].GetString()),
	                  Value(acl["group"].GetString()), Value(webhdfs::FormatPermission(permission)),
	                  Value::BOOLEAN(acl["stickyBit"].GetBool()), StringList(acl["entries"])});
}

static bool DecodeHex(const string &text, string &out) {
	auto start = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2 : 0;
	if ((text.size() - start) % 2 != 0) {
		return false;
	}
	out.clear();
	for (idx_t i = start; i < text.size(); i += 2) {
		auto hex = text.substr(i, 2);
		char *end = nullptr;
		auto byte = strtol(hex.c_str(), &end, 16);
		if (!end || *end != '\0') {
			return false;
		}
		out += static_cast<char>(byte);
	}
	return true;
}

static unique_ptr<RowSource> MakeXattrs(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	vector<std::pair<string, string>> query {{"encoding", "hex"}};
	auto names = call.Named("names");
	if (!names.IsNull()) {
		for (auto &name : ListValue::GetChildren(names)) {
			if (!name.IsNull()) {
				query.emplace_back("xattr.name", name.ToString());
			}
		}
	}
	auto doc = ParseJson(client->GetJson("GETXATTRS", parsed.path, query), "GETXATTRS");
	auto xattrs = doc->Root()["XAttrs"];
	auto url = client->UrlFor(parsed.path);
	vector<Row> rows;
	for (idx_t i = 0; i < xattrs.Size(); i++) {
		auto xattr = xattrs.At(i);
		string bytes;
		auto value = xattr["value"];
		Value blob(LogicalType::BLOB);
		if (!value.IsNull() && !value.IsMissing() && DecodeHex(value.GetString(), bytes)) {
			blob = Value::BLOB_RAW(bytes);
		}
		rows.push_back({Value(url), Value(xattr["name"].GetString()), blob});
	}
	return make_uniq<VectorRowSource>(std::move(rows));
}

static unique_ptr<RowSource> MakeStoragePolicy(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETSTORAGEPOLICY", parsed.path), "GETSTORAGEPOLICY");
	auto policy = doc->Root()["BlockStoragePolicy"];
	return SingleRow({Value(client->UrlFor(parsed.path)), Value::INTEGER(policy["id"].GetInt()),
	                  Value(policy["name"].GetString()), StringList(policy["storageTypes"]),
	                  StringList(policy["creationFallbacks"]), StringList(policy["replicationFallbacks"]),
	                  Value::BOOLEAN(policy["copyOnCreateFile"].GetBool())});
}

static unique_ptr<RowSource> MakeSnapshots(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETSNAPSHOTLIST", parsed.path), "GETSNAPSHOTLIST");
	auto snapshots = doc->Root()["SnapshotList"];
	vector<Row> rows;
	for (idx_t i = 0; i < snapshots.Size(); i++) {
		auto snapshot = snapshots.At(i);
		auto full_path = snapshot["fullPath"].GetString();
		auto status = webhdfs::ParseFileStatusObject(snapshot["dirStatus"]);
		rows.push_back({Value(client->UrlFor(full_path)), Value(webhdfs::BaseName(full_path)),
		                Value::INTEGER(snapshot["snapshotID"].GetInt()), TimestampValue(status.modification_time)});
	}
	return make_uniq<VectorRowSource>(std::move(rows));
}

static unique_ptr<RowSource> MakeSnapshottableDirectories(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETSNAPSHOTTABLEDIRECTORYLIST", "/"), "GETSNAPSHOTTABLEDIRECTORYLIST");
	auto list = doc->Root()["SnapshottableDirectoryList"];
	vector<Row> rows;
	for (idx_t i = 0; i < list.Size(); i++) {
		auto entry = list.At(i);
		auto status = webhdfs::ParseFileStatusObject(entry["dirStatus"]);
		auto path = webhdfs::JoinPath(entry["parentFullPath"].GetString("/"), status.path_suffix);
		rows.push_back({Value(client->UrlFor(path)), Value::INTEGER(entry["snapshotNumber"].GetInt()),
		                Value::INTEGER(entry["snapshotQuota"].GetInt()), Value(status.owner), Value(status.group),
		                Value(webhdfs::FormatPermission(status.permission)),
		                TimestampValue(status.modification_time)});
	}
	return make_uniq<VectorRowSource>(std::move(rows));
}

static unique_ptr<RowSource> MakeServerDefaults(MetaCall &call) {
	webhdfs::WebHdfsUrl parsed;
	auto client = call.Client(parsed);
	auto doc = ParseJson(client->GetJson("GETSERVERDEFAULTS", "/"), "GETSERVERDEFAULTS");
	auto defaults = doc->Root()["FsServerDefaults"];
	return SingleRow({Value(parsed.Prefix()), Value::BIGINT(defaults["blockSize"].GetInt()),
	                  Value::INTEGER(defaults["bytesPerChecksum"].GetInt()),
	                  Value::INTEGER(defaults["writePacketSize"].GetInt()),
	                  Value::INTEGER(defaults["replication"].GetInt()),
	                  Value::INTEGER(defaults["fileBufferSize"].GetInt()),
	                  Value::BOOLEAN(defaults["encryptDataTransfer"].GetBool()),
	                  Value::BIGINT(defaults["trashInterval"].GetInt()),
	                  Value::INTEGER(defaults["checksumType"].GetInt()),
	                  OptionalString(defaults["keyProviderUri"].GetString()),
	                  Value::INTEGER(defaults["defaultStoragePolicyId"].GetInt())});
}

static vector<MetaFunctionDef> &FunctionDefs() {
	static vector<MetaFunctionDef> defs = {
	    {"webhdfs_ls",
	     EntryColumns(),
	     MakeLs,
	     {{"include_hidden", LogicalType::BOOLEAN}, {"skip_permission_errors", LogicalType::BOOLEAN}}},
	    {"webhdfs_stat", EntryColumns(), MakeStat, {}},
	    {"webhdfs_content_summary",
	     {{"path", LogicalType::VARCHAR},
	      {"length", LogicalType::BIGINT},
	      {"file_count", LogicalType::BIGINT},
	      {"directory_count", LogicalType::BIGINT},
	      {"space_consumed", LogicalType::BIGINT},
	      {"quota", LogicalType::BIGINT},
	      {"space_quota", LogicalType::BIGINT},
	      {"erasure_coding_policy", LogicalType::VARCHAR}},
	     MakeContentSummary,
	     {}},
	    {"webhdfs_quota_usage",
	     {{"path", LogicalType::VARCHAR},
	      {"file_and_directory_count", LogicalType::BIGINT},
	      {"quota", LogicalType::BIGINT},
	      {"space_consumed", LogicalType::BIGINT},
	      {"space_quota", LogicalType::BIGINT},
	      {"type_quotas", TypeQuotaType()}},
	     MakeQuotaUsage,
	     {}},
	    {"webhdfs_block_locations",
	     {{"path", LogicalType::VARCHAR},
	      {"offset", LogicalType::BIGINT},
	      {"length", LogicalType::BIGINT},
	      {"hosts", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"names", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"topology_paths", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"storage_types", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"corrupt", LogicalType::BOOLEAN}},
	     MakeBlockLocations,
	     {{"offset", LogicalType::BIGINT}, {"length", LogicalType::BIGINT}}},
	    {"webhdfs_checksum",
	     {{"path", LogicalType::VARCHAR},
	      {"algorithm", LogicalType::VARCHAR},
	      {"bytes", LogicalType::VARCHAR},
	      {"length", LogicalType::INTEGER}},
	     MakeChecksum,
	     {}},
	    {"webhdfs_acl",
	     {{"path", LogicalType::VARCHAR},
	      {"owner", LogicalType::VARCHAR},
	      {"group", LogicalType::VARCHAR},
	      {"permissions", LogicalType::VARCHAR},
	      {"sticky_bit", LogicalType::BOOLEAN},
	      {"entries", LogicalType::LIST(LogicalType::VARCHAR)}},
	     MakeAcl,
	     {}},
	    {"webhdfs_xattrs",
	     {{"path", LogicalType::VARCHAR}, {"name", LogicalType::VARCHAR}, {"value", LogicalType::BLOB}},
	     MakeXattrs,
	     {{"names", LogicalType::LIST(LogicalType::VARCHAR)}}},
	    {"webhdfs_storage_policy",
	     {{"path", LogicalType::VARCHAR},
	      {"id", LogicalType::INTEGER},
	      {"name", LogicalType::VARCHAR},
	      {"storage_types", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"creation_fallbacks", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"replication_fallbacks", LogicalType::LIST(LogicalType::VARCHAR)},
	      {"copy_on_create_file", LogicalType::BOOLEAN}},
	     MakeStoragePolicy,
	     {}},
	    {"webhdfs_snapshots",
	     {{"path", LogicalType::VARCHAR},
	      {"name", LogicalType::VARCHAR},
	      {"snapshot_id", LogicalType::INTEGER},
	      {"created", LogicalType::TIMESTAMP_TZ}},
	     MakeSnapshots,
	     {}},
	    {"webhdfs_snapshottable_directories",
	     {{"path", LogicalType::VARCHAR},
	      {"snapshot_count", LogicalType::INTEGER},
	      {"snapshot_quota", LogicalType::INTEGER},
	      {"owner", LogicalType::VARCHAR},
	      {"group", LogicalType::VARCHAR},
	      {"permissions", LogicalType::VARCHAR},
	      {"last_modified", LogicalType::TIMESTAMP_TZ}},
	     MakeSnapshottableDirectories,
	     {}},
	    {"webhdfs_server_defaults",
	     {{"endpoint", LogicalType::VARCHAR},
	      {"block_size", LogicalType::BIGINT},
	      {"bytes_per_checksum", LogicalType::INTEGER},
	      {"write_packet_size", LogicalType::INTEGER},
	      {"replication", LogicalType::INTEGER},
	      {"file_buffer_size", LogicalType::INTEGER},
	      {"encrypt_data_transfer", LogicalType::BOOLEAN},
	      {"trash_interval", LogicalType::BIGINT},
	      {"checksum_type", LogicalType::INTEGER},
	      {"key_provider_uri", LogicalType::VARCHAR},
	      {"default_storage_policy_id", LogicalType::INTEGER}},
	     MakeServerDefaults,
	     {}},
	};
	return defs;
}

//===--------------------------------------------------------------------===//
// In-out table function scaffolding
//===--------------------------------------------------------------------===//

struct MetaFunctionInfo : public TableFunctionInfo {
	MetaFunctionInfo(WebHdfsFileSystem &fs_p, const MetaFunctionDef &def_p) : fs(fs_p), def(def_p) {
	}
	WebHdfsFileSystem &fs;
	const MetaFunctionDef &def;
};

struct MetaBindData : public TableFunctionData {
	MetaBindData(WebHdfsFileSystem &fs_p, const MetaFunctionDef &def_p) : fs(fs_p), def(def_p) {
	}
	WebHdfsFileSystem &fs;
	const MetaFunctionDef &def;
	named_argument_map_t named;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<MetaBindData>(fs, def);
		result->named = named;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MetaBindData>();
		return &def == &other.def;
	}
};

struct MetaLocalState : public LocalTableFunctionState {
	idx_t row = 0;
	unique_ptr<RowSource> source;
};

static unique_ptr<FunctionData> MetaBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &info = input.info->Cast<MetaFunctionInfo>();
	for (auto &column : info.def.columns) {
		names.emplace_back(column.first);
		return_types.push_back(column.second);
	}
	auto result = make_uniq<MetaBindData>(info.fs, info.def);
	for (auto &entry : input.named_parameters) {
		result->named.insert(entry.first, entry.second);
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> MetaInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<GlobalTableFunctionState>();
}

static unique_ptr<LocalTableFunctionState> MetaInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                         GlobalTableFunctionState *global_state) {
	return make_uniq<MetaLocalState>();
}

static OperatorResultType MetaInOut(ExecutionContext &context, TableFunctionInput &data, DataChunk &input,
                                    DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<MetaBindData>();
	auto &state = data.local_state->Cast<MetaLocalState>();
	while (true) {
		if (!state.source) {
			if (state.row >= input.size()) {
				state.row = 0;
				return OperatorResultType::NEED_MORE_INPUT;
			}
			auto url = input.GetValue(0, state.row);
			if (url.IsNull()) {
				state.row++;
				continue;
			}
			MetaCall call {bind_data.fs, context.client, url.ToString(), bind_data.named};
			state.source = bind_data.def.make(call);
		}
		vector<Row> rows;
		if (!state.source->Next(rows, STANDARD_VECTOR_SIZE)) {
			state.source.reset();
			state.row++;
			continue;
		}
		for (idx_t r = 0; r < rows.size(); r++) {
			for (idx_t c = 0; c < rows[r].size(); c++) {
				output.SetValue(c, r, rows[r][c]);
			}
		}
		output.SetCardinality(rows.size());
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
}

//===--------------------------------------------------------------------===//
// Scalar functions
//===--------------------------------------------------------------------===//

struct WebHdfsScalarInfo : public ScalarFunctionInfo {
	explicit WebHdfsScalarInfo(WebHdfsFileSystem &fs_p) : fs(fs_p) {
	}
	WebHdfsFileSystem &fs;
};

static WebHdfsFileSystem &ScalarFs(ExpressionState &state) {
	auto &expr = state.expr.Cast<BoundFunctionExpression>();
	return expr.Function().GetExtraFunctionInfo().Cast<WebHdfsScalarInfo>().fs;
}

static void ExistsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = ScalarFs(state);
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t url) {
		webhdfs::WebHdfsUrl parsed;
		auto client = fs.CreateClient(url.GetString(), ContextOpener(context), parsed);
		FileStatus status;
		return client->TryGetFileStatus(parsed.path, status);
	});
}

static void PathFunction(DataChunk &args, ExpressionState &state, Vector &result, const char *op, bool root) {
	auto &fs = ScalarFs(state);
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t url) {
		webhdfs::WebHdfsUrl parsed;
		auto client = fs.CreateClient(url.GetString(), ContextOpener(context), parsed);
		auto doc = ParseJson(client->GetJson(op, root ? "/" : parsed.path), op);
		auto path = doc->Root()["Path"].GetString();
		return StringVector::AddString(result, client->UrlFor(path));
	});
}

static void HomeDirectoryFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	PathFunction(args, state, result, "GETHOMEDIRECTORY", true);
}

static void TrashRootFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	PathFunction(args, state, result, "GETTRASHROOT", false);
}

//===--------------------------------------------------------------------===//
// webhdfs_namenodes()
//===--------------------------------------------------------------------===//

struct NameNodesInfo : public TableFunctionInfo {
	explicit NameNodesInfo(WebHdfsFileSystem &fs_p) : fs(fs_p) {
	}
	WebHdfsFileSystem &fs;
};

struct NameNodesBindData : public TableFunctionData {
	explicit NameNodesBindData(WebHdfsFileSystem &fs_p) : fs(fs_p) {
	}
	WebHdfsFileSystem &fs;
};

struct NameNodesState : public GlobalTableFunctionState {
	vector<Row> rows;
	idx_t position = 0;
};

static unique_ptr<FunctionData> NameNodesBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	vector<std::pair<const char *, LogicalType>> columns = {
	    {"endpoint", LogicalType::VARCHAR},         {"namenode", LogicalType::VARCHAR},
	    {"state", LogicalType::VARCHAR},            {"active", LogicalType::BOOLEAN},
	    {"requests", LogicalType::UBIGINT},         {"last_error", LogicalType::VARCHAR},
	    {"datanode_requests", LogicalType::UBIGINT}, {"auth_mode", LogicalType::VARCHAR},
	    {"namenode_source", LogicalType::VARCHAR},  {"secret_name", LogicalType::VARCHAR},
	    {"batched_listing", LogicalType::BOOLEAN}};
	for (auto &column : columns) {
		names.emplace_back(column.first);
		return_types.push_back(column.second);
	}
	return make_uniq<NameNodesBindData>(input.info->Cast<NameNodesInfo>().fs);
}

static unique_ptr<GlobalTableFunctionState> NameNodesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<NameNodesBindData>();
	auto state = make_uniq<NameNodesState>();
	for (auto &snapshot : bind_data.fs.EndpointSnapshots()) {
		for (auto &nn : snapshot.namenodes) {
			auto batched = snapshot.batch_listing < 0 ? Value(LogicalType::BOOLEAN)
			                                          : Value::BOOLEAN(snapshot.batch_listing == 1);
			state->rows.push_back({Value(snapshot.key), Value(nn.first), Value(nn.second.state),
			                       Value::BOOLEAN(nn.first == snapshot.active), Value::UBIGINT(nn.second.requests),
			                       OptionalString(nn.second.last_error), Value::UBIGINT(snapshot.datanode_requests),
			                       Value(snapshot.auth_mode), Value(snapshot.namenode_source),
			                       OptionalString(snapshot.secret_name), batched});
		}
	}
	return std::move(state);
}

static void NameNodesFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<NameNodesState>();
	idx_t count = 0;
	while (state.position < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.position++];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterWebHdfsFunctions(ExtensionLoader &loader, WebHdfsFileSystem &fs) {
	for (auto &def : FunctionDefs()) {
		TableFunction function(Identifier(def.name), {LogicalType::VARCHAR}, nullptr, MetaBind, MetaInitGlobal,
		                       MetaInitLocal);
		function.in_out_function = MetaInOut;
		for (auto &keyword : def.keywords) {
			function.GetSignature().AddKeywordOnly(Identifier(keyword.first), keyword.second, Value(keyword.second));
		}
		function.function_info = make_shared_ptr<MetaFunctionInfo>(fs, def);
		loader.RegisterFunction(function);
	}

	TableFunction namenodes(Identifier("webhdfs_namenodes"), {}, NameNodesFunction, NameNodesBind, NameNodesInit);
	namenodes.function_info = make_shared_ptr<NameNodesInfo>(fs);
	loader.RegisterFunction(namenodes);

	auto info = make_shared_ptr<WebHdfsScalarInfo>(fs);
	ScalarFunction exists(Identifier("webhdfs_exists"), {LogicalType::VARCHAR}, LogicalType::BOOLEAN, ExistsFunction);
	ScalarFunction home(Identifier("webhdfs_home_directory"), {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                    HomeDirectoryFunction);
	ScalarFunction trash(Identifier("webhdfs_trash_root"), {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                     TrashRootFunction);
	for (auto function : {&exists, &home, &trash}) {
		function->SetVolatile();
		function->SetExtraFunctionInfo(info);
		loader.RegisterFunction(*function);
	}
}

} // namespace duckdb

#include "webhdfs_json.hpp"

#include "yyjson.hpp"

#include <cstdlib>
#include <vector>

using namespace duckdb_yyjson; // NOLINT

namespace webhdfs {

static yyjson_val *V(void *val) {
	return static_cast<yyjson_val *>(val);
}

bool JsonValue::IsNull() const {
	return val && yyjson_is_null(V(val));
}
bool JsonValue::IsObject() const {
	return val && yyjson_is_obj(V(val));
}
bool JsonValue::IsArray() const {
	return val && yyjson_is_arr(V(val));
}
bool JsonValue::IsString() const {
	return val && yyjson_is_str(V(val));
}
bool JsonValue::IsNumber() const {
	return val && yyjson_is_num(V(val));
}
bool JsonValue::IsBool() const {
	return val && yyjson_is_bool(V(val));
}

JsonValue JsonValue::operator[](const char *key) const {
	if (!IsObject()) {
		return JsonValue();
	}
	return JsonValue(yyjson_obj_get(V(val), key));
}

size_t JsonValue::Size() const {
	return IsArray() ? yyjson_arr_size(V(val)) : 0;
}

JsonValue JsonValue::At(size_t index) const {
	if (!IsArray()) {
		return JsonValue();
	}
	return JsonValue(yyjson_arr_get(V(val), index));
}

void JsonValue::ForEachMember(const std::function<void(const std::string &, JsonValue)> &fn) const {
	if (!IsObject()) {
		return;
	}
	yyjson_obj_iter iter;
	yyjson_obj_iter_init(V(val), &iter);
	yyjson_val *key;
	while ((key = yyjson_obj_iter_next(&iter))) {
		auto value = yyjson_obj_iter_get_val(key);
		fn(std::string(yyjson_get_str(key), yyjson_get_len(key)), JsonValue(value));
	}
}

std::string JsonValue::GetString(const std::string &default_value) const {
	if (!val) {
		return default_value;
	}
	if (yyjson_is_str(V(val))) {
		return std::string(yyjson_get_str(V(val)), yyjson_get_len(V(val)));
	}
	if (yyjson_is_null(V(val))) {
		return default_value;
	}
	return ToJson();
}

int64_t JsonValue::GetInt(int64_t default_value) const {
	if (!val) {
		return default_value;
	}
	if (yyjson_is_sint(V(val))) {
		return yyjson_get_sint(V(val));
	}
	if (yyjson_is_uint(V(val))) {
		return static_cast<int64_t>(yyjson_get_uint(V(val)));
	}
	if (yyjson_is_real(V(val))) {
		return static_cast<int64_t>(yyjson_get_real(V(val)));
	}
	if (yyjson_is_str(V(val))) {
		char *end = nullptr;
		auto str = GetString();
		auto result = std::strtoll(str.c_str(), &end, 10);
		if (end && *end == '\0' && !str.empty()) {
			return result;
		}
	}
	return default_value;
}

double JsonValue::GetDouble(double default_value) const {
	if (!val || !yyjson_is_num(V(val))) {
		return default_value;
	}
	if (yyjson_is_real(V(val))) {
		return yyjson_get_real(V(val));
	}
	return static_cast<double>(GetInt());
}

bool JsonValue::GetBool(bool default_value) const {
	if (!val || !yyjson_is_bool(V(val))) {
		return default_value;
	}
	return yyjson_get_bool(V(val));
}

std::string JsonValue::ToJson() const {
	if (!val) {
		return "";
	}
	size_t len = 0;
	char *text = yyjson_val_write(V(val), 0, &len);
	if (!text) {
		return "";
	}
	std::string result(text, len);
	free(text);
	return result;
}

std::unique_ptr<JsonDocument> JsonDocument::Parse(const std::string &text, std::string &error) {
	// yyjson_read_opts only writes to its input with YYJSON_READ_INSITU; it takes a mutable buffer anyway
	std::vector<char> buffer(text.begin(), text.end());
	yyjson_read_err err;
	auto doc = yyjson_read_opts(buffer.data(), buffer.size(), YYJSON_READ_NOFLAG, nullptr, &err);
	if (!doc) {
		error = std::string("invalid JSON: ") + (err.msg ? err.msg : "unknown error");
		return nullptr;
	}
	std::unique_ptr<JsonDocument> result(new JsonDocument());
	result->doc = doc;
	return result;
}

JsonDocument::~JsonDocument() {
	if (doc) {
		yyjson_doc_free(static_cast<yyjson_doc *>(doc));
	}
}

JsonValue JsonDocument::Root() const {
	return JsonValue(yyjson_doc_get_root(static_cast<yyjson_doc *>(doc)));
}

uint16_t ParseOctalPermission(const std::string &text) {
	uint16_t result = 0;
	for (auto c : text) {
		if (c < '0' || c > '7') {
			return 0;
		}
		result = static_cast<uint16_t>(result * 8 + (c - '0'));
	}
	return static_cast<uint16_t>(result & 07777);
}

std::string FormatPermission(uint16_t permission) {
	const char *chars = "rwxrwxrwx";
	std::string result(9, '-');
	for (int i = 0; i < 9; i++) {
		if (permission & (1 << (8 - i))) {
			result[i] = chars[i];
		}
	}
	if (permission & 01000) {
		result[8] = (permission & 1) ? 't' : 'T';
	}
	return result;
}

FileStatus ParseFileStatusObject(JsonValue obj) {
	FileStatus status;
	status.path_suffix = obj["pathSuffix"].GetString();
	auto type = obj["type"].GetString();
	status.is_dir = type == "DIRECTORY";
	status.is_symlink = type == "SYMLINK";
	status.length = obj["length"].GetInt(0);
	status.modification_time = obj["modificationTime"].GetInt(0);
	status.access_time = obj["accessTime"].GetInt(0);
	status.owner = obj["owner"].GetString();
	status.group = obj["group"].GetString();
	auto permission = obj["permission"];
	status.permission =
	    ParseOctalPermission(permission.IsString() ? permission.GetString() : std::to_string(permission.GetInt(0)));
	if (!status.is_dir) {
		status.replication = static_cast<int32_t>(obj["replication"].GetInt(-1));
		status.block_size = obj["blockSize"].GetInt(-1);
	}
	status.file_id = obj["fileId"].GetInt(-1);
	status.children_num = obj["childrenNum"].GetInt(-1);
	status.storage_policy = static_cast<int32_t>(obj["storagePolicy"].GetInt(-1));
	auto ec = obj["ecPolicy"];
	if (ec.IsString()) {
		status.ec_policy = ec.GetString();
	} else if (ec.IsObject()) {
		status.ec_policy = ec["name"].GetString();
	}
	status.symlink = obj["symlink"].GetString();
	return status;
}

static std::unique_ptr<JsonDocument> ParseOrError(const std::string &body, std::string &error) {
	return JsonDocument::Parse(body, error);
}

bool ParseFileStatus(const std::string &body, FileStatus &out, std::string &error) {
	auto doc = ParseOrError(body, error);
	if (!doc) {
		return false;
	}
	auto obj = doc->Root()["FileStatus"];
	if (!obj.IsObject()) {
		error = "response has no FileStatus object";
		return false;
	}
	out = ParseFileStatusObject(obj);
	return true;
}

bool ParseFileStatusList(const std::string &body, std::vector<FileStatus> &out, int64_t &remaining,
                         std::string &error) {
	auto doc = ParseOrError(body, error);
	if (!doc) {
		return false;
	}
	auto root = doc->Root();
	remaining = 0;
	JsonValue statuses;
	if (root["DirectoryListing"].IsObject()) {
		auto listing = root["DirectoryListing"];
		remaining = listing["remainingEntries"].GetInt(0);
		statuses = listing["partialListing"]["FileStatuses"]["FileStatus"];
	} else if (root["FileStatuses"].IsObject()) {
		statuses = root["FileStatuses"]["FileStatus"];
	} else {
		error = "response has no FileStatuses or DirectoryListing object";
		return false;
	}
	if (statuses.IsMissing() || statuses.IsNull()) {
		return true;
	}
	if (!statuses.IsArray()) {
		error = "FileStatus is not an array";
		return false;
	}
	out.reserve(out.size() + statuses.Size());
	for (size_t i = 0; i < statuses.Size(); i++) {
		out.push_back(ParseFileStatusObject(statuses.At(i)));
	}
	return true;
}

bool ParseRemoteException(const std::string &body, RemoteException &out) {
	std::string error;
	auto doc = JsonDocument::Parse(body, error);
	if (!doc) {
		return false;
	}
	auto obj = doc->Root()["RemoteException"];
	if (!obj.IsObject()) {
		return false;
	}
	out.exception = obj["exception"].GetString();
	out.java_class_name = obj["javaClassName"].GetString();
	out.message = obj["message"].GetString();
	return true;
}

bool ParseBooleanResult(const std::string &body, bool &out, std::string &error) {
	auto doc = ParseOrError(body, error);
	if (!doc) {
		return false;
	}
	auto value = doc->Root()["boolean"];
	if (!value.IsBool()) {
		error = "response has no boolean result";
		return false;
	}
	out = value.GetBool();
	return true;
}

bool ParseLocation(const std::string &body, std::string &out) {
	std::string error;
	auto doc = JsonDocument::Parse(body, error);
	if (!doc) {
		return false;
	}
	auto location = doc->Root()["Location"];
	if (!location.IsString()) {
		return false;
	}
	out = location.GetString();
	return true;
}

bool ParseToken(const std::string &body, std::string &out, std::string &error) {
	auto doc = ParseOrError(body, error);
	if (!doc) {
		return false;
	}
	auto token = doc->Root()["Token"]["urlString"];
	if (!token.IsString()) {
		error = "response has no delegation token";
		return false;
	}
	out = token.GetString();
	return true;
}

bool ParseLongResult(const std::string &body, int64_t &out, std::string &error) {
	auto doc = ParseOrError(body, error);
	if (!doc) {
		return false;
	}
	auto value = doc->Root()["long"];
	if (!value.IsNumber()) {
		error = "response has no long result";
		return false;
	}
	out = value.GetInt();
	return true;
}

} // namespace webhdfs

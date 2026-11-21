#include "webhdfs_json.hpp"

#include "yyjson.hpp"

#include <cerrno>
#include <cstdint>
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

bool JsonValue::TryGetInt(int64_t &out, int64_t default_value) const {
	out = default_value;
	if (!val) {
		return true;
	}
	if (yyjson_is_sint(V(val))) {
		out = yyjson_get_sint(V(val));
		return true;
	}
	if (yyjson_is_uint(V(val))) {
		auto value = yyjson_get_uint(V(val));
		if (value > static_cast<uint64_t>(INT64_MAX)) {
			return false;
		}
		out = static_cast<int64_t>(value);
		return true;
	}
	if (yyjson_is_real(V(val))) {
		auto value = yyjson_get_real(V(val));
		// [-2^63, 2^63) is exactly the doubles whose truncation fits; NaN fails both comparisons
		if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
			return false;
		}
		out = static_cast<int64_t>(value);
		return true;
	}
	if (yyjson_is_str(V(val))) {
		auto str = GetString();
		if (str.empty()) {
			return true;
		}
		char *end = nullptr;
		errno = 0;
		auto result = std::strtoll(str.c_str(), &end, 10);
		if (!end || *end != '\0') {
			// not a number: treat like a missing value
			return true;
		}
		if (errno == ERANGE) {
			return false;
		}
		out = result;
		return true;
	}
	return true;
}

int64_t JsonValue::GetInt(int64_t default_value) const {
	int64_t result;
	return TryGetInt(result, default_value) ? result : default_value;
}

double JsonValue::GetDouble(double default_value) const {
	if (!val || !yyjson_is_num(V(val))) {
		return default_value;
	}
	return yyjson_get_num(V(val));
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

bool IsValidPathSuffix(const std::string &suffix) {
	if (suffix == "." || suffix == "..") {
		return false;
	}
	return suffix.find('/') == std::string::npos && suffix.find('\0') == std::string::npos;
}

// Reads an integer field; false (with `error` set) when the server sent a number that doesn't fit int64.
static bool ReadInt(JsonValue obj, const char *field, int64_t default_value, int64_t &out, std::string &error) {
	auto value = obj[field];
	if (!value.TryGetInt(out, default_value)) {
		error = std::string("field \"") + field + "\" is out of range: " + value.ToJson();
		return false;
	}
	return true;
}

static bool ReadInt32(JsonValue obj, const char *field, int64_t default_value, int32_t &out, std::string &error) {
	int64_t value;
	if (!ReadInt(obj, field, default_value, value, error)) {
		return false;
	}
	if (value < INT32_MIN || value > INT32_MAX) {
		error = std::string("field \"") + field + "\" is out of range: " + std::to_string(value);
		return false;
	}
	out = static_cast<int32_t>(value);
	return true;
}

static bool ReadEpochMs(JsonValue obj, const char *field, int64_t &out, std::string &error) {
	if (!ReadInt(obj, field, 0, out, error)) {
		return false;
	}
	if (out < MIN_EPOCH_MS || out > MAX_EPOCH_MS) {
		error = std::string("field \"") + field + "\" is not a valid time: " + std::to_string(out);
		return false;
	}
	return true;
}

static bool ParseFileStatusFields(JsonValue obj, FileStatus &status, std::string &error) {
	if (!obj.IsObject()) {
		error = "FileStatus is not an object";
		return false;
	}
	status.path_suffix = obj["pathSuffix"].GetString();
	if (!IsValidPathSuffix(status.path_suffix)) {
		error = "invalid pathSuffix \"" + status.path_suffix + "\"";
		return false;
	}
	auto type = obj["type"].GetString();
	status.is_dir = type == "DIRECTORY";
	status.is_symlink = type == "SYMLINK";
	if (!ReadInt(obj, "length", 0, status.length, error)) {
		return false;
	}
	if (status.length < 0) {
		error = "negative length " + std::to_string(status.length);
		return false;
	}
	if (!ReadEpochMs(obj, "modificationTime", status.modification_time, error) ||
	    !ReadEpochMs(obj, "accessTime", status.access_time, error)) {
		return false;
	}
	status.owner = obj["owner"].GetString();
	status.group = obj["group"].GetString();
	auto permission = obj["permission"];
	status.permission =
	    ParseOctalPermission(permission.IsString() ? permission.GetString() : std::to_string(permission.GetInt(0)));
	if (!status.is_dir) {
		if (!ReadInt32(obj, "replication", -1, status.replication, error) ||
		    !ReadInt(obj, "blockSize", -1, status.block_size, error)) {
			return false;
		}
	}
	if (!ReadInt(obj, "fileId", -1, status.file_id, error) ||
	    !ReadInt(obj, "childrenNum", -1, status.children_num, error) ||
	    !ReadInt32(obj, "storagePolicy", -1, status.storage_policy, error)) {
		return false;
	}
	if (status.file_id < 0) {
		// no inode id: one value for "none", so every tag built from it agrees
		status.file_id = -1;
	}
	auto ec = obj["ecPolicy"];
	if (ec.IsString()) {
		status.ec_policy = ec.GetString();
	} else if (ec.IsObject()) {
		status.ec_policy = ec["name"].GetString();
	}
	status.symlink = obj["symlink"].GetString();
	return true;
}

bool ParseFileStatusObject(JsonValue obj, FileStatus &out, std::string &error) {
	FileStatus status;
	if (!ParseFileStatusFields(obj, status, error)) {
		if (!status.path_suffix.empty() && IsValidPathSuffix(status.path_suffix)) {
			error = "FileStatus of \"" + status.path_suffix + "\": " + error;
		} else {
			error = "FileStatus: " + error;
		}
		return false;
	}
	out = std::move(status);
	return true;
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
	return ParseFileStatusObject(obj, out, error);
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
		if (!listing["remainingEntries"].TryGetInt(remaining, 0)) {
			error = "remainingEntries is out of range";
			return false;
		}
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
		FileStatus status;
		if (!ParseFileStatusObject(statuses.At(i), status, error)) {
			return false;
		}
		out.push_back(std::move(status));
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
	if (!value.TryGetInt(out)) {
		error = "long result is out of range: " + value.ToJson();
		return false;
	}
	return true;
}

static int HexDigit(char c) {
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

bool DecodeHex(const std::string &text, std::string &out) {
	size_t start = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2 : 0;
	if ((text.size() - start) % 2 != 0) {
		return false;
	}
	std::string result;
	for (size_t i = start; i < text.size(); i += 2) {
		auto high = HexDigit(text[i]);
		auto low = HexDigit(text[i + 1]);
		if (high < 0 || low < 0) {
			return false;
		}
		result += static_cast<char>(high * 16 + low);
	}
	out = std::move(result);
	return true;
}

std::string VersionTag(int64_t file_id, int64_t modification_time_ms, uint64_t length) {
	return std::to_string(file_id) + ":" + std::to_string(modification_time_ms) + ":" + std::to_string(length);
}

bool ListingEtag(const FileStatus &status, std::string &out) {
	if (status.length < 0) {
		return false;
	}
	out = VersionTag(status.file_id, status.modification_time, static_cast<uint64_t>(status.length));
	return true;
}

} // namespace webhdfs

#pragma once

// A small read-only JSON DOM over the yyjson copy that ships with DuckDB, plus parsers for the WebHDFS
// responses the filesystem needs. No DuckDB headers beyond yyjson, so it can be unit tested on its own.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace webhdfs {

class JsonDocument;

//! A borrowed view of one JSON value; valid while its JsonDocument lives. A default (null) view is "missing".
class JsonValue {
public:
	JsonValue() = default;
	explicit JsonValue(void *val) : val(val) {
	}

	bool IsMissing() const {
		return val == nullptr;
	}
	bool IsNull() const;
	bool IsObject() const;
	bool IsArray() const;
	bool IsString() const;
	bool IsNumber() const;
	bool IsBool() const;

	//! Object member (missing view if absent or not an object)
	JsonValue operator[](const char *key) const;
	//! Array element count (0 if not an array)
	size_t Size() const;
	//! Array element (missing view if out of range)
	JsonValue At(size_t index) const;
	//! Visit an object's members in order
	void ForEachMember(const std::function<void(const std::string &, JsonValue)> &fn) const;

	std::string GetString(const std::string &default_value = "") const;
	//! An integer (number or numeric string); `default_value` when missing, null or not numeric. A number that
	//! doesn't fit int64 also gives `default_value`: use TryGetInt where that must be an error.
	int64_t GetInt(int64_t default_value = 0) const;
	//! Like GetInt, but returns false when the value is a number (or numeric string) outside the int64 range
	bool TryGetInt(int64_t &out, int64_t default_value = 0) const;
	double GetDouble(double default_value = 0) const;
	bool GetBool(bool default_value = false) const;
	//! Serialize this value back to JSON text
	std::string ToJson() const;

private:
	void *val = nullptr;
};

class JsonDocument {
public:
	//! Parse JSON text. Returns nullptr (and sets error) if the text is not valid JSON.
	static std::unique_ptr<JsonDocument> Parse(const std::string &text, std::string &error);
	~JsonDocument();

	JsonValue Root() const;

private:
	JsonDocument() = default;
	void *doc = nullptr;
};

//! One HDFS FileStatus object
struct FileStatus {
	std::string path_suffix;
	bool is_dir = false;
	bool is_symlink = false;
	int64_t length = 0;
	//! epoch milliseconds, within [MIN_EPOCH_MS, MAX_EPOCH_MS]
	int64_t modification_time = 0;
	//! 0 when HDFS doesn't track it (directories, or dfs.namenode.accesstime.precision = 0)
	int64_t access_time = 0;
	std::string owner;
	std::string group;
	//! the 9 permission bits plus sticky bit (octal 1777 max)
	uint16_t permission = 0;
	int32_t replication = -1;
	int64_t block_size = -1;
	int64_t file_id = -1;
	int64_t children_num = -1;
	int32_t storage_policy = -1;
	std::string ec_policy;
	std::string symlink;
};

struct RemoteException {
	std::string exception;
	std::string java_class_name;
	std::string message;
};

//! Range of epoch milliseconds a FileStatus time may have: DuckDB's TIMESTAMP range (290309-12-22 BC to 294247-01-10).
//! The lower end is not -MAX_EPOCH_MS: DuckDB's smallest TIMESTAMP is a whole day.
constexpr int64_t MIN_EPOCH_MS = -9223372022400000;
constexpr int64_t MAX_EPOCH_MS = INT64_MAX / 1000 - 1;

//! A name the server may return as a pathSuffix: no '/' or NUL, not "." or "..". "" (the listed path is a file) is
//! valid.
bool IsValidPathSuffix(const std::string &suffix);
//! Parse a FileStatus JSON object. Returns false (and sets `error`) when a field is out of range, the length is
//! negative or the pathSuffix is not a plain name.
bool ParseFileStatusObject(JsonValue obj, FileStatus &out, std::string &error);
//! {"FileStatus": {...}}
bool ParseFileStatus(const std::string &body, FileStatus &out, std::string &error);
//! LISTSTATUS ({"FileStatuses":{"FileStatus":[...]}}) or LISTSTATUS_BATCH
//! ({"DirectoryListing":{"partialListing":{"FileStatuses":...},"remainingEntries":n}}).
//! `remaining` is set to the batch's remainingEntries (0 for LISTSTATUS).
bool ParseFileStatusList(const std::string &body, std::vector<FileStatus> &out, int64_t &remaining, std::string &error);
//! {"RemoteException":{...}}; false if the body is not one
bool ParseRemoteException(const std::string &body, RemoteException &out);
//! {"boolean": true|false}
bool ParseBooleanResult(const std::string &body, bool &out, std::string &error);
//! {"Location": "..."} (noredirect responses)
bool ParseLocation(const std::string &body, std::string &out);
//! {"Token":{"urlString":"..."}}
bool ParseToken(const std::string &body, std::string &out, std::string &error);
//! {"long": n}
bool ParseLongResult(const std::string &body, int64_t &out, std::string &error);
//! "755" / "1777" -> bits
uint16_t ParseOctalPermission(const std::string &text);
//! bits -> "rwxr-xr-x" (with t/T for the sticky bit)
std::string FormatPermission(uint16_t permission);
//! An xattr value in GETXATTRS' hex encoding ("0x" and an even number of hex digits); false for anything else
bool DecodeHex(const std::string &text, std::string &out);

} // namespace webhdfs

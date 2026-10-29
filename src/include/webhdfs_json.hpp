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
	int64_t GetInt(int64_t default_value = 0) const;
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
	//! epoch milliseconds
	int64_t modification_time = 0;
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

//! Parse a FileStatus JSON object
FileStatus ParseFileStatusObject(JsonValue obj);
//! {"FileStatus": {...}}
bool ParseFileStatus(const std::string &body, FileStatus &out, std::string &error);
//! LISTSTATUS ({"FileStatuses":{"FileStatus":[...]}}) or LISTSTATUS_BATCH
//! ({"DirectoryListing":{"partialListing":{"FileStatuses":...},"remainingEntries":n}}).
//! `remaining` is set to the batch's remainingEntries (0 for LISTSTATUS).
bool ParseFileStatusList(const std::string &body, std::vector<FileStatus> &out, int64_t &remaining,
                         std::string &error);
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

} // namespace webhdfs

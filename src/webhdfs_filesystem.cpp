#include "webhdfs_filesystem.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "webhdfs_glob.hpp"
#include "webhdfs_file_list.hpp"

#include <algorithm>
#include <set>
#include <cstring>

namespace duckdb {

using webhdfs::FileStatus;
using webhdfs::WebHdfsUrl;

//===--------------------------------------------------------------------===//
// File handle
//===--------------------------------------------------------------------===//

WebHdfsFileHandle::WebHdfsFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags,
                                     shared_ptr<WebHdfsClient> client_p, string hdfs_path_p)
    : FileHandle(fs, path, std::move(flags)), client(std::move(client_p)), hdfs_path(std::move(hdfs_path_p)) {
}

WebHdfsFileHandle::~WebHdfsFileHandle() {
	if (closed) {
		return;
	}
	// Some core writers drop the handle without calling Close() (EXPORT DATABASE's schema.sql/load.sql, log
	// storage), as local files allow, so a handle destroyed normally still commits its data. A handle destroyed
	// while an exception unwinds, or after a failed request, belongs to a failed write: it is aborted instead, and
	// a file it created is removed. Neither path lets an exception escape the destructor.
	if (writing && !failed && !Exception::UncaughtException()) {
		try {
			Close();
			return;
		} catch (...) { // NOLINT: a destructor can't throw; fall through to the cleanup below
		}
	}
	Abort();
}

void WebHdfsFileHandle::CheckWritable() const {
	if (!writing || closed) {
		throw IOException("WebHDFS: \"%s\" is not open for writing", path);
	}
	if (failed) {
		throw IOException("WebHDFS: a previous write to \"%s\" failed; the file is incomplete", path);
	}
}

void WebHdfsFileHandle::SendBuffer() {
	try {
		if (!created) {
			// The file is only ours to remove once the CREATE succeeded. A failed CREATE may have failed before the
			// NameNode acted (standby, connection error) or on another client's lease, and the existing file then
			// belongs to someone else. A CREATE that replaced the target and failed on the DataNode leg leaves an
			// incomplete file behind instead.
			client->Create(hdfs_path, write_buffer.data(), write_buffer.size(), overwrite);
			created = true;
			delete_on_abort = true;
		} else {
			client->Append(hdfs_path, write_buffer.data(), write_buffer.size());
		}
	} catch (...) {
		// The server may have applied the chunk: never send it again
		failed = true;
		write_buffer.clear();
		throw;
	}
	written += write_buffer.size();
	write_buffer.clear();
}

void WebHdfsFileHandle::Flush() {
	if (!writing || write_buffer.empty()) {
		return;
	}
	CheckWritable();
	SendBuffer();
}

void WebHdfsFileHandle::Close() {
	if (closed) {
		return;
	}
	if (!writing) {
		closed = true;
		return;
	}
	CheckWritable();
	if (!created || !write_buffer.empty()) {
		// the CREATE of a file that fit in one chunk also creates empty files
		SendBuffer();
	}
	// only now: if the final request failed, Abort() (or the destructor) still removes the incomplete file
	closed = true;
}

void WebHdfsFileHandle::Abort() {
	if (closed) {
		return;
	}
	closed = true;
	write_buffer.clear();
	if (writing && delete_on_abort) {
		try {
			client->Delete(hdfs_path, false);
		} catch (...) { // NOLINT: best effort cleanup of an abandoned write
		}
	}
}

//===--------------------------------------------------------------------===//
// File system
//===--------------------------------------------------------------------===//

WebHdfsFileSystem::WebHdfsFileSystem(DatabaseInstance &db_p) : db(db_p) {
}

WebHdfsFileSystem::~WebHdfsFileSystem() = default;

const webhdfs::HadoopConfig &WebHdfsFileSystem::SiteConfig() {
	lock_guard<mutex> guard(site_lock);
	if (!site_loaded) {
		auto dir = webhdfs::HadoopConfig::ConfigDirectory();
		webhdfs::HadoopConfig loaded;
		if (!dir.empty()) {
			string error;
			for (auto name : {"core-site.xml", "hdfs-site.xml"}) {
				if (!loaded.LoadFile(dir + "/" + name, error)) {
					throw InvalidConfigurationException("webhdfs: could not read Hadoop configuration: %s", error);
				}
			}
		}
		site = std::move(loaded);
		site_loaded = true;
	}
	return site;
}

WebHdfsEndpoint &WebHdfsFileSystem::GetEndpoint(const string &key) {
	lock_guard<mutex> guard(endpoints_lock);
	auto entry = endpoints.find(key);
	if (entry == endpoints.end()) {
		entry = endpoints.emplace(key, make_uniq<WebHdfsEndpoint>(key)).first;
		entry->second->SetSmallFileCacheSize(small_file_cache_size);
	}
	return *entry->second;
}

vector<WebHdfsEndpoint::Snapshot> WebHdfsFileSystem::EndpointSnapshots() {
	vector<WebHdfsEndpoint *> list;
	{
		lock_guard<mutex> guard(endpoints_lock);
		for (auto &entry : endpoints) {
			list.push_back(entry.second.get());
		}
	}
	vector<WebHdfsEndpoint::Snapshot> result;
	for (auto endpoint : list) {
		result.push_back(endpoint->GetSnapshot());
	}
	std::sort(result.begin(), result.end(),
	          [](const WebHdfsEndpoint::Snapshot &a, const WebHdfsEndpoint::Snapshot &b) { return a.key < b.key; });
	return result;
}

void WebHdfsFileSystem::SetSmallFileCacheSize(idx_t bytes) {
	lock_guard<mutex> guard(endpoints_lock);
	small_file_cache_size = bytes;
	small_file_cache_size_set = true;
	for (auto &entry : endpoints) {
		entry.second->SetSmallFileCacheSize(bytes);
	}
}

void WebHdfsFileSystem::SeedSmallFileCacheSize(idx_t bytes) {
	lock_guard<mutex> guard(endpoints_lock);
	if (small_file_cache_size_set) {
		return;
	}
	small_file_cache_size = bytes;
	// another connection may already have opened a cluster while the extension was loading
	for (auto &entry : endpoints) {
		entry.second->SetSmallFileCacheSize(bytes);
	}
}

shared_ptr<WebHdfsClient> WebHdfsFileSystem::CreateClient(const string &url, optional_ptr<FileOpener> opener,
                                                          WebHdfsUrl &parsed, FileType access_type) {
	string error;
	if (!webhdfs::ParseWebHdfsUrl(url, parsed, error)) {
		throw IOException("Invalid WebHDFS URL \"%s\": %s", url, error);
	}
	// The webhdfs_* functions reach this without the virtual file system, and Kerberos contacts the KDC before the
	// first HTTP request: check what both would check, before any of it.
	CheckAccess(parsed, access_type);
	auto settings = WebHdfsSettings::Load(db, opener);
	auto config = ResolveWebHdfsConfig(db, opener, parsed, SiteConfig(), settings);
	auto &endpoint = GetEndpoint(parsed.Prefix());
	return make_shared_ptr<WebHdfsClient>(db, opener, parsed, std::move(config), std::move(settings), endpoint,
	                                      access_type);
}

void WebHdfsFileSystem::CheckAccess(const WebHdfsUrl &parsed, FileType access_type) {
	if (db.GetFileSystem().SubSystemIsDisabled(GetName())) {
		throw PermissionException("File system %s has been disabled by configuration", GetName());
	}
	if (!db.config.CanAccessFile(parsed.ToString(), access_type)) {
		throw PermissionException("Cannot access \"%s\" - file system operations are disabled by configuration",
		                          parsed.ToString());
	}
}

bool WebHdfsFileSystem::TryResolveConfig(const string &key, optional_ptr<FileOpener> opener, WebHdfsConfig &out) {
	WebHdfsUrl parsed;
	string error;
	if (!webhdfs::ParseWebHdfsUrl(key, parsed, error)) {
		return false;
	}
	// the same gate as CreateClient: a disabled or refused file system reads no secrets or Hadoop configuration
	try {
		CheckAccess(parsed, FileType::FILE_TYPE_DIR);
	} catch (PermissionException &) {
		return false;
	}
	// the site configuration was loaded by the request that created the endpoint, and an invalid setting is an error
	// here as for a request. A secret whose values don't resolve (InvalidConfigurationException, InvalidInputException,
	// or ConversionException for a mistyped secret value) shows as NULL; anything else is an error too
	auto &site = SiteConfig();
	auto settings = WebHdfsSettings::Load(db, opener);
	try {
		out = ResolveWebHdfsConfig(db, opener, parsed, site, settings);
		return true;
	} catch (InvalidConfigurationException &) {
		return false;
	} catch (InvalidInputException &) {
		return false;
	} catch (ConversionException &) {
		return false;
	}
}

#ifdef _WIN32
//! The directory name ListFiles is handing to its callback on this thread, if any (see ListEntry)
static thread_local const string *listed_directory_name = nullptr;
#endif

bool WebHdfsFileSystem::CanHandleFile(const string &fpath) {
#ifdef _WIN32
	if (listed_directory_name && fpath == *listed_directory_name) {
		return true;
	}
#endif
	return webhdfs::HasWebHdfsScheme(fpath) || !webhdfs::SchemeRootOf(fpath).empty();
}

string WebHdfsFileSystem::CanonicalizePath(const string &path, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	string error;
	if (!webhdfs::ParseWebHdfsUrl(path, parsed, error)) {
		// not a URL we would request (e.g. '..' above the root): opening it fails with the parse error
		return path;
	}
	return parsed.ToString();
}

void WebHdfsFileSystem::ListClusterNames(const string &scheme, optional_ptr<FileOpener> opener,
                                         const std::function<void(OpenFileInfo &info)> &callback) {
	std::set<string> names;
	for (auto &authority : WebHdfsSecretAuthorities(db, opener, scheme)) {
		names.insert(authority);
	}
	for (auto &nameservice : webhdfs::SplitCommaList(SiteConfig().Get("dfs.nameservices"))) {
		names.insert(StringUtil::Lower(nameservice));
	}
	auto prefix = scheme + "://";
	{
		lock_guard<mutex> guard(endpoints_lock);
		for (auto &entry : endpoints) {
			if (StringUtil::StartsWith(entry.first, prefix) && entry.first.size() > prefix.size()) {
				names.insert(entry.first.substr(prefix.size()));
			}
		}
	}
	FileStatus status;
	status.is_dir = true;
	for (auto &name : names) {
		auto info = MakeOpenFileInfo(name, status);
		callback(info);
	}
}

OpenFileInfo WebHdfsFileSystem::MakeOpenFileInfo(const string &url, const FileStatus &status) {
	OpenFileInfo info(url);
	info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	auto &options = info.extended_info->options;
	options.emplace("type", Value(status.is_dir ? "directory" : "file"));
	options.emplace("file_size", Value::BIGINT(status.length));
	options.emplace("last_modified", Value::TIMESTAMP(WebHdfsTimestamp(status.modification_time)));
	if (status.file_id >= 0) {
		options.emplace("file_id", Value::BIGINT(status.file_id));
	}
	// what GetVersionTag returns for the opened file: caches that validate listed files (Parquet metadata) compare them
	string etag;
	if (webhdfs::ListingEtag(status, etag)) {
		options.emplace("etag", Value(etag));
	}
	return info;
}

//! A file length from the server, checked: a bad value is the server's error, not an internal one
static idx_t FileLength(const FileStatus &status, const string &url) {
	if (status.length < 0) {
		throw IOException("WebHDFS: the server reported an invalid length (%lld) for \"%s\"", status.length, url);
	}
	return UnsafeNumericCast<idx_t>(status.length);
}

unique_ptr<FileHandle> WebHdfsFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                   optional_ptr<FileOpener> opener) {
	return OpenFileExtended(OpenFileInfo(path), std::move(flags), opener);
}

unique_ptr<FileHandle> WebHdfsFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                           optional_ptr<FileOpener> opener) {
	if (flags.Compression() != FileCompressionType::UNCOMPRESSED) {
		throw InternalException("WebHdfsFileSystem: compressed files are opened through the virtual file system");
	}
	WebHdfsUrl parsed;
	auto client = CreateClient(file.path, opener, parsed, FileType::FILE_TYPE_REGULAR);
	auto handle = make_uniq<WebHdfsFileHandle>(*this, parsed.ToString(), flags, client, parsed.path);

	if (flags.OpenForWriting()) {
		if (flags.OpenForReading()) {
			throw NotImplementedException("WebHDFS files can't be opened for reading and writing at the same time");
		}
		handle->writing = true;
		handle->append = flags.OpenForAppending();
		// HDFS checks existence atomically at CREATE: the check below only gives the early error / nullptr
		handle->overwrite = !(flags.ExclusiveCreate() || flags.ReturnNullIfExists());
		if (handle->append || flags.ReturnNullIfExists() || flags.ExclusiveCreate()) {
			FileStatus status;
			bool exists = client->TryGetFileStatus(parsed.path, status);
			if (exists && status.is_dir) {
				throw IOException("Cannot open \"%s\" for writing: it is a directory", file.path);
			}
			if (exists && flags.ReturnNullIfExists()) {
				// the handle wrote nothing: its destructor must not create the file
				handle->closed = true;
				return nullptr;
			}
			if (exists && flags.ExclusiveCreate()) {
				throw IOException("Cannot create \"%s\": the file already exists", file.path);
			}
			if (exists && handle->append) {
				handle->created = true;
				handle->written = FileLength(status, file.path);
				handle->length = handle->written;
				handle->position = handle->length;
			}
		}
		return std::move(handle);
	}

	// reading: size and mtime from the listing that found the file, else one GETFILESTATUS
	idx_t file_size = 0;
	timestamp_t last_modified;
	string type;
	if (file.extended_info && file.extended_info->TryGetOption("file_size", file_size) &&
	    file.extended_info->TryGetOption("last_modified", last_modified) &&
	    (!file.extended_info->TryGetOption("type", type) || type == "file")) {
		handle->length = file_size;
		handle->modification_time_ms = Timestamp::GetEpochMs(last_modified);
		// absent when the server sent none: the handle keeps -1, as the listing's etag does
		auto file_id = file.extended_info->options.find("file_id");
		if (file_id != file.extended_info->options.end()) {
			handle->file_id = file_id->second.GetValue<int64_t>();
		}
		return std::move(handle);
	}
	FileStatus status;
	if (!client->TryGetFileStatus(parsed.path, status)) {
		if (flags.ReturnNullIfNotExists()) {
			return nullptr;
		}
		throw IOException("Cannot open file \"%s\": No such file or directory", file.path);
	}
	if (status.is_dir) {
		throw IOException("Cannot open file \"%s\": it is a directory", file.path);
	}
	handle->length = FileLength(status, file.path);
	handle->modification_time_ms = status.modification_time;
	handle->file_id = status.file_id;
	return std::move(handle);
}

void WebHdfsFileSystem::Read(FileHandle &handle_p, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &handle = handle_p.Cast<WebHdfsFileHandle>();
	if (handle.writing) {
		throw NotImplementedException("WebHDFS: cannot read from a file opened for writing");
	}
	if (nr_bytes <= 0) {
		return;
	}
	auto bytes = NumericCast<idx_t>(nr_bytes);
	if (bytes > handle.length || location > handle.length - bytes) {
		throw IOException("WebHDFS: read of %llu bytes at offset %llu is past the end of \"%s\" (%llu bytes)", bytes,
		                  location, handle.path, handle.length);
	}
	auto out = static_cast<data_ptr_t>(buffer);
	auto read_ahead = handle.client->Settings().read_buffer_size;
	if (read_ahead == 0 || bytes >= read_ahead) {
		handle.client->ReadRange(handle.hdfs_path, location, bytes, out, GetVersionTag(handle));
		return;
	}
	{
		lock_guard<mutex> guard(handle.buffer_lock);
		if (location >= handle.read_buffer_offset &&
		    location + bytes <= handle.read_buffer_offset + handle.read_buffer_length) {
			memcpy(out, handle.read_buffer.data() + (location - handle.read_buffer_offset), bytes);
			return;
		}
	}
	// A miss: fetch the window without holding the lock, so that other threads reading this handle (Parquet
	// reads one handle from several threads) are not stalled behind a network round trip. Two threads may fetch
	// the same window at once; the last one to finish installs its copy.
	// Choose the window to fetch. Formats with a footer (Parquet) read the end of a file first and then jump
	// back, so a small file is fetched whole and a read near the end of a larger one takes the file's tail.
	idx_t start = location;
	if (handle.length <= read_ahead) {
		start = 0;
	} else if (location + bytes > handle.length - read_ahead) {
		// the window must still contain the whole read
		start = MinValue<idx_t>(location, handle.length - read_ahead);
	}
	auto fetch = MinValue<idx_t>(read_ahead, handle.length - start);
	vector<data_t> window(fetch);
	auto whole_file = start == 0 && fetch == handle.length;
	auto version = GetVersionTag(handle);
	auto &endpoint = handle.client->Endpoint();
	shared_ptr<const string> cached;
	string cache_key;
	if (whole_file) {
		cache_key = handle.client->CacheKey(handle.hdfs_path, version);
		cached = endpoint.GetSmallFile(cache_key);
	}
	if (cached && cached->size() == fetch) {
		memcpy(window.data(), cached->data(), fetch);
	} else {
		handle.client->ReadRange(handle.hdfs_path, start, fetch, window.data(), version);
		if (whole_file) {
			endpoint.PutSmallFile(cache_key, make_shared_ptr<const string>(const_char_ptr_cast(window.data()), fetch));
		}
	}
	memcpy(out, window.data() + (location - start), bytes);
	lock_guard<mutex> guard(handle.buffer_lock);
	handle.read_buffer = std::move(window);
	handle.read_buffer_offset = start;
	handle.read_buffer_length = fetch;
}

int64_t WebHdfsFileSystem::Read(FileHandle &handle_p, void *buffer, int64_t nr_bytes) {
	auto &handle = handle_p.Cast<WebHdfsFileHandle>();
	if (handle.position >= handle.length || nr_bytes <= 0) {
		return 0;
	}
	auto bytes = MinValue<idx_t>(NumericCast<idx_t>(nr_bytes), handle.length - handle.position);
	Read(handle, buffer, NumericCast<int64_t>(bytes), handle.position);
	handle.position += bytes;
	return NumericCast<int64_t>(bytes);
}

int64_t WebHdfsFileSystem::Write(FileHandle &handle_p, void *buffer, int64_t nr_bytes) {
	auto &handle = handle_p.Cast<WebHdfsFileHandle>();
	handle.CheckWritable();
	if (nr_bytes <= 0) {
		return 0;
	}
	auto bytes = NumericCast<idx_t>(nr_bytes);
	auto data = static_cast<const_data_ptr_t>(buffer);
	handle.write_buffer.insert(handle.write_buffer.end(), data, data + bytes);
	handle.length += bytes;
	handle.position = handle.length;
	if (handle.write_buffer.size() >= handle.client->Settings().write_chunk_size) {
		handle.Flush();
	}
	return nr_bytes;
}

void WebHdfsFileSystem::Write(FileHandle &handle_p, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &handle = handle_p.Cast<WebHdfsFileHandle>();
	if (location != handle.length) {
		throw NotImplementedException("WebHDFS files are append-only: cannot write at offset %llu of \"%s\" (current "
		                              "length %llu)",
		                              location, handle.path, handle.length);
	}
	Write(handle_p, buffer, nr_bytes);
}

int64_t WebHdfsFileSystem::GetFileSize(FileHandle &handle) {
	return NumericCast<int64_t>(handle.Cast<WebHdfsFileHandle>().length);
}

timestamp_t WebHdfsFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return WebHdfsTimestamp(handle.Cast<WebHdfsFileHandle>().modification_time_ms);
}

string WebHdfsFileSystem::GetVersionTag(FileHandle &handle_p) {
	auto &handle = handle_p.Cast<WebHdfsFileHandle>();
	return webhdfs::VersionTag(handle.file_id, handle.modification_time_ms, handle.length);
}

FileType WebHdfsFileSystem::GetFileType(FileHandle &handle) {
	return FileType::FILE_TYPE_REGULAR;
}

void WebHdfsFileSystem::Seek(FileHandle &handle_p, idx_t location) {
	auto &handle = handle_p.Cast<WebHdfsFileHandle>();
	if (handle.writing && location != handle.length) {
		throw NotImplementedException("WebHDFS files are append-only: cannot seek while writing");
	}
	handle.position = location;
}

idx_t WebHdfsFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<WebHdfsFileHandle>().position;
}

void WebHdfsFileSystem::Reset(FileHandle &handle) {
	Seek(handle, 0);
}

void WebHdfsFileSystem::FileSync(FileHandle &handle) {
	handle.Cast<WebHdfsFileHandle>().Flush();
}

void WebHdfsFileSystem::AbortFileWrite(FileHandle &handle) {
	handle.Cast<WebHdfsFileHandle>().Abort();
}

void WebHdfsFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	throw NotImplementedException("WebHDFS: truncating files is not supported");
}

bool WebHdfsFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(filename, opener, parsed, FileType::FILE_TYPE_REGULAR);
	FileStatus status;
	return client->TryGetFileStatus(parsed.path, status) && !status.is_dir;
}

bool WebHdfsFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed, FileType::FILE_TYPE_DIR);
	FileStatus status;
	return client->TryGetFileStatus(parsed.path, status) && status.is_dir;
}

void WebHdfsFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed, FileType::FILE_TYPE_DIR);
	if (!client->Mkdirs(parsed.path)) {
		throw IOException("WebHDFS: could not create directory \"%s\"", directory);
	}
}

bool WebHdfsFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	// MKDIRS always creates missing parents, so SINGLE behaves like RECURSIVE
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed, FileType::FILE_TYPE_DIR);
	FileStatus status;
	if (client->TryGetFileStatus(parsed.path, status)) {
		if (!status.is_dir) {
			throw IOException("WebHDFS: cannot create directory \"%s\": a file with that name exists", directory);
		}
		return false;
	}
	if (!client->Mkdirs(parsed.path)) {
		throw IOException("WebHDFS: could not create directory \"%s\"", directory);
	}
	return true;
}

void WebHdfsFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed, FileType::FILE_TYPE_DIR);
	FileStatus status;
	if (!client->TryGetFileStatus(parsed.path, status)) {
		return;
	}
	if (!status.is_dir) {
		throw IOException("WebHDFS: cannot remove directory \"%s\": it is a file", directory);
	}
	client->Delete(parsed.path, true);
}

bool WebHdfsFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed, FileType::FILE_TYPE_DIR);
	FileStatus status;
	if (!client->TryGetFileStatus(parsed.path, status) || !status.is_dir) {
		return false;
	}
	if (options.mode == RemoveDirectoryMode::RECURSIVE) {
		return client->Delete(parsed.path, true);
	}
	try {
		return client->Delete(parsed.path, false);
	} catch (IOException &) {
		// PathIsNotEmptyDirectoryException
		return false;
	}
}

void WebHdfsFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	if (!TryRemoveFile(filename, opener)) {
		throw IOException("Could not remove file \"%s\": No such file or directory", filename);
	}
}

bool WebHdfsFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(filename, opener, parsed, FileType::FILE_TYPE_REGULAR);
	FileStatus status;
	if (!client->TryGetFileStatus(parsed.path, status)) {
		return false;
	}
	if (status.is_dir) {
		throw IOException("Could not remove file \"%s\": it is a directory", filename);
	}
	return client->Delete(parsed.path, false);
}

void WebHdfsFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	WebHdfsUrl source_url;
	auto client = CreateClient(source, opener, source_url, FileType::FILE_TYPE_REGULAR);
	WebHdfsUrl target_url;
	string error;
	if (!webhdfs::ParseWebHdfsUrl(target, target_url, error)) {
		throw IOException("Invalid WebHDFS URL \"%s\": %s", target, error);
	}
	if (source_url.Prefix() != target_url.Prefix()) {
		throw NotImplementedException("WebHDFS can only move files within one cluster (\"%s\" -> \"%s\")", source,
		                              target);
	}
	// RENAME with renameoptions=OVERWRITE runs the NameNode's rename2: one request that replaces a file target
	// atomically (what StorageManager and COPY ... USE_TMP_FILE expect of MoveFile) and fails for a directory
	// target, where the old rename would move the source into the directory.
	// The source's status first: a missing source is an error, and its length tells a replayed RENAME (source moved
	// by an earlier attempt whose response was lost) from an old target that was already there.
	FileStatus source_status;
	if (!client->TryGetFileStatus(source_url.path, source_status)) {
		throw IOException("Could not move \"%s\": No such file or directory", source);
	}
	auto outcome = client->RenameOverwrite(source_url.path, target_url.path);
	switch (outcome) {
	case WebHdfsRenameOutcome::RENAMED:
	case WebHdfsRenameOutcome::LEGACY_TRUE:
		// LEGACY_TRUE: a gateway (HttpFS) ignored the option and ran the old rename, which succeeded. With a
		// directory target that old rename moved the source into it; gateways give no atomic alternative.
		return;
	case WebHdfsRenameOutcome::OPTION_REJECTED:
		// A gateway that rejects the option: fall back to the old rename (and the non-atomic replace below)
		if (client->Rename(source_url.path, target_url.path)) {
			return;
		}
		break;
	case WebHdfsRenameOutcome::NOT_FOUND:
	case WebHdfsRenameOutcome::LEGACY_FALSE:
		break;
	}
	// The source is gone or the rename was refused. A RENAME replayed after a lost response (by the HTTP retries
	// or a NameNode failover) finds the source already moved: source gone and target present means it succeeded.
	FileStatus status;
	if (!client->TryGetFileStatus(source_url.path, status)) {
		FileStatus target_status;
		if (client->TryGetFileStatus(target_url.path, target_status) && !target_status.is_dir &&
		    target_status.is_dir == source_status.is_dir && target_status.length == source_status.length) {
			return;
		}
		throw IOException("Could not move \"%s\": No such file or directory", source);
	}
	if (outcome == WebHdfsRenameOutcome::NOT_FOUND) {
		throw IOException("Could not move \"%s\" to \"%s\": the target directory does not exist", source, target);
	}
	// Fallback for gateways without rename2, whose old rename refuses an existing target: delete it, then rename.
	// This is not atomic: if the second rename fails, the old target is already gone.
	FileStatus target_status;
	if (client->TryGetFileStatus(target_url.path, target_status)) {
		if (target_status.is_dir) {
			throw IOException("Could not move \"%s\" to \"%s\": the target is a directory", source, target);
		}
		client->Delete(target_url.path, false);
		if (client->Rename(source_url.path, target_url.path)) {
			return;
		}
	}
	throw IOException("Could not move \"%s\" to \"%s\"", source, target);
}

//! Hands one listed entry to a ListFiles callback. On Windows, autocomplete ends a directory suggestion with
//! fs.PathSeparator(name) for the bare entry name, which the virtual file system routes to the local file system
//! ("\\"). While the callback runs for a directory, this file system claims exactly that name on this thread
//! (CanHandleFile), so the separator is "/". A workaround until autocomplete asks for the listed directory's
//! separator; elsewhere the local separator is "/" already.
static void ListEntry(const std::function<void(OpenFileInfo &info)> &callback, OpenFileInfo &info) {
#ifdef _WIN32
	if (!FileSystem::IsDirectory(info)) {
		callback(info);
		return;
	}
	auto previous = listed_directory_name;
	listed_directory_name = &info.path;
	try {
		callback(info);
	} catch (...) {
		listed_directory_name = previous;
		throw;
	}
	listed_directory_name = previous;
#else
	callback(info);
#endif
}

bool WebHdfsFileSystem::ListFilesExtended(const string &directory,
                                          const std::function<void(OpenFileInfo &info)> &callback,
                                          optional_ptr<FileOpener> opener) {
	auto scheme = webhdfs::SchemeRootOf(directory);
	if (!scheme.empty()) {
		// "webhdfs:/" is what autocomplete lists for "webhdfs://<Tab>": offer the clusters this session knows
		ListClusterNames(scheme, opener, [&](OpenFileInfo &info) { ListEntry(callback, info); });
		return true;
	}
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed, FileType::FILE_TYPE_DIR);
	return client->TryListDirectory(parsed.path, [&](vector<FileStatus> &entries) {
		for (auto &entry : entries) {
			if (entry.path_suffix.empty()) {
				continue;
			}
			auto info = MakeOpenFileInfo(entry.path_suffix, entry);
			ListEntry(callback, info);
		}
	});
}

bool WebHdfsFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                                  FileOpener *opener) {
	return ListFilesExtended(
	    directory, [&](OpenFileInfo &info) { callback(info.path, FileSystem::IsDirectory(info)); }, opener);
}

vector<OpenFileInfo> WebHdfsFileSystem::Glob(const string &path, FileOpener *opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(path, opener, parsed, FileType::FILE_TYPE_REGULAR);
	vector<OpenFileInfo> result;
	if (!webhdfs::HasGlobCharacters(parsed.path)) {
		FileStatus status;
		if (client->TryGetFileStatus(parsed.path, status) && !status.is_dir) {
			result.push_back(MakeOpenFileInfo(parsed.ToString(), status));
		}
		return result;
	}
	auto &settings = client->Settings();
	WebHdfsWalkOptions options;
	options.include_hidden = settings.include_hidden;
	options.skip_permission_errors = settings.skip_permission_errors;
	options.parallelism = settings.list_parallelism;
	options.files_only = true;
	WebHdfsWalk walk(client, parsed.path, options);
	for (auto &entry : walk.All()) {
		result.push_back(MakeOpenFileInfo(parsed.Prefix() + entry.path, entry.status));
	}
	std::sort(result.begin(), result.end());
	return result;
}

unique_ptr<MultiFileList> WebHdfsFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                               optional_ptr<FileOpener> opener) {
	vector<string> globs {path};
	return make_uniq<WebHdfsFileList>(*this, FileOpener::TryGetClientContext(opener), opener, std::move(globs));
}

} // namespace duckdb

#include "webhdfs_filesystem.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "webhdfs_glob.hpp"
#include "webhdfs_file_list.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

using webhdfs::FileStatus;
using webhdfs::WebHdfsUrl;

//===--------------------------------------------------------------------===//
// File handle
//===--------------------------------------------------------------------===//

WebHdfsFileHandle::WebHdfsFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags,
                                     shared_ptr<WebHdfsClient> client_p, string hdfs_path_p)
    : FileHandle(fs, path, flags), client(std::move(client_p)), hdfs_path(std::move(hdfs_path_p)) {
}

WebHdfsFileHandle::~WebHdfsFileHandle() {
	try {
		Close();
	} catch (...) { // NOLINT: destructors must not throw; an explicit Close() reports the error
	}
}

void WebHdfsFileHandle::Flush() {
	if (!writing || write_buffer.empty()) {
		return;
	}
	if (!created) {
		client->Create(hdfs_path, write_buffer.data(), write_buffer.size(), overwrite);
		created = true;
	} else {
		client->Append(hdfs_path, write_buffer.data(), write_buffer.size());
	}
	written += write_buffer.size();
	write_buffer.clear();
}

void WebHdfsFileHandle::Close() {
	if (closed) {
		return;
	}
	closed = true;
	if (!writing) {
		return;
	}
	if (!created) {
		// also creates empty files
		client->Create(hdfs_path, write_buffer.data(), write_buffer.size(), overwrite);
		created = true;
		written += write_buffer.size();
		write_buffer.clear();
		return;
	}
	Flush();
}

void WebHdfsFileHandle::Abort() {
	if (closed) {
		return;
	}
	closed = true;
	write_buffer.clear();
	if (writing && created && !append) {
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

shared_ptr<WebHdfsClient> WebHdfsFileSystem::CreateClient(const string &url, optional_ptr<FileOpener> opener,
                                                          WebHdfsUrl &parsed) {
	string error;
	if (!webhdfs::ParseWebHdfsUrl(url, parsed, error)) {
		throw IOException("Invalid WebHDFS URL \"%s\": %s", url, error);
	}
	auto settings = WebHdfsSettings::Load(db, opener);
	auto config = ResolveWebHdfsConfig(db, opener, parsed, SiteConfig(), settings);
	auto &endpoint = GetEndpoint(parsed.Prefix());
	return make_shared_ptr<WebHdfsClient>(db, opener, parsed, std::move(config), std::move(settings), endpoint);
}

bool WebHdfsFileSystem::CanHandleFile(const string &fpath) {
	return webhdfs::HasWebHdfsScheme(fpath);
}

OpenFileInfo WebHdfsFileSystem::MakeOpenFileInfo(const string &url, const FileStatus &status) {
	OpenFileInfo info(url);
	info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	auto &options = info.extended_info->options;
	options.emplace("type", Value(status.is_dir ? "directory" : "file"));
	options.emplace("file_size", Value::BIGINT(status.length));
	options.emplace("last_modified", Value::TIMESTAMP(WebHdfsTimestamp(status.modification_time)));
	return info;
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
	auto client = CreateClient(file.path, opener, parsed);
	auto handle = make_uniq<WebHdfsFileHandle>(*this, parsed.ToString(), flags, client, parsed.path);

	if (flags.OpenForWriting()) {
		if (flags.OpenForReading()) {
			throw NotImplementedException("WebHDFS files can't be opened for reading and writing at the same time");
		}
		handle->writing = true;
		handle->append = flags.OpenForAppending();
		handle->overwrite = true;
		if (handle->append || flags.ReturnNullIfExists() || flags.ExclusiveCreate()) {
			FileStatus status;
			bool exists = client->TryGetFileStatus(parsed.path, status);
			if (exists && status.is_dir) {
				throw IOException("Cannot open \"%s\" for writing: it is a directory", file.path);
			}
			if (exists && flags.ReturnNullIfExists()) {
				return nullptr;
			}
			if (exists && flags.ExclusiveCreate()) {
				throw IOException("Cannot create \"%s\": the file already exists", file.path);
			}
			if (exists && handle->append) {
				handle->created = true;
				handle->written = NumericCast<idx_t>(status.length);
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
	handle->length = NumericCast<idx_t>(status.length);
	handle->modification_time_ms = status.modification_time;
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
	if (location + bytes > handle.length) {
		throw IOException("WebHDFS: read of %llu bytes at offset %llu is past the end of \"%s\" (%llu bytes)", bytes,
		                  location, handle.path, handle.length);
	}
	auto out = static_cast<data_ptr_t>(buffer);
	auto read_ahead = handle.client->Settings().read_buffer_size;
	if (read_ahead == 0 || bytes >= read_ahead) {
		handle.client->ReadRange(handle.hdfs_path, location, bytes, out, GetVersionTag(handle));
		return;
	}
	lock_guard<mutex> guard(handle.buffer_lock);
	if (location < handle.read_buffer_offset ||
	    location + bytes > handle.read_buffer_offset + handle.read_buffer_length) {
		auto fetch = MinValue<idx_t>(read_ahead, handle.length - location);
		if (handle.read_buffer.size() < fetch) {
			handle.read_buffer.resize(fetch);
		}
		handle.read_buffer_length = 0;
		handle.client->ReadRange(handle.hdfs_path, location, fetch, handle.read_buffer.data(), GetVersionTag(handle));
		handle.read_buffer_offset = location;
		handle.read_buffer_length = fetch;
	}
	memcpy(out, handle.read_buffer.data() + (location - handle.read_buffer_offset), bytes);
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
	if (!handle.writing || handle.closed) {
		throw IOException("WebHDFS: \"%s\" is not open for writing", handle.path);
	}
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
	return std::to_string(handle.modification_time_ms) + ":" + std::to_string(handle.length);
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
	auto client = CreateClient(filename, opener, parsed);
	FileStatus status;
	return client->TryGetFileStatus(parsed.path, status) && !status.is_dir;
}

bool WebHdfsFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed);
	FileStatus status;
	return client->TryGetFileStatus(parsed.path, status) && status.is_dir;
}

void WebHdfsFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed);
	if (!client->Mkdirs(parsed.path)) {
		throw IOException("WebHDFS: could not create directory \"%s\"", directory);
	}
}

bool WebHdfsFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                                optional_ptr<FileOpener> opener) {
	// MKDIRS always creates missing parents, so SINGLE behaves like RECURSIVE
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed);
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
	auto client = CreateClient(directory, opener, parsed);
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
	auto client = CreateClient(directory, opener, parsed);
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
	auto client = CreateClient(filename, opener, parsed);
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
	auto client = CreateClient(source, opener, source_url);
	WebHdfsUrl target_url;
	string error;
	if (!webhdfs::ParseWebHdfsUrl(target, target_url, error)) {
		throw IOException("Invalid WebHDFS URL \"%s\": %s", target, error);
	}
	if (source_url.Prefix() != target_url.Prefix()) {
		throw NotImplementedException("WebHDFS can only move files within one cluster (\"%s\" -> \"%s\")", source,
		                              target);
	}
	if (client->Rename(source_url.path, target_url.path)) {
		return;
	}
	// HDFS refuses to rename onto an existing file; DuckDB expects the target to be replaced
	FileStatus status;
	if (!client->TryGetFileStatus(source_url.path, status)) {
		throw IOException("Could not move \"%s\": No such file or directory", source);
	}
	FileStatus target_status;
	if (client->TryGetFileStatus(target_url.path, target_status) && !target_status.is_dir) {
		client->Delete(target_url.path, false);
		if (client->Rename(source_url.path, target_url.path)) {
			return;
		}
	}
	throw IOException("Could not move \"%s\" to \"%s\"", source, target);
}

bool WebHdfsFileSystem::ListFilesExtended(const string &directory,
                                          const std::function<void(OpenFileInfo &info)> &callback,
                                          optional_ptr<FileOpener> opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(directory, opener, parsed);
	FileStatus status;
	if (!client->TryGetFileStatus(parsed.path, status) || !status.is_dir) {
		return false;
	}
	client->ListStatus(parsed.path, [&](vector<FileStatus> &entries) {
		for (auto &entry : entries) {
			if (entry.path_suffix.empty()) {
				continue;
			}
			auto info = MakeOpenFileInfo(entry.path_suffix, entry);
			callback(info);
		}
	});
	return true;
}

bool WebHdfsFileSystem::ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
                                  FileOpener *opener) {
	return ListFilesExtended(
	    directory, [&](OpenFileInfo &info) { callback(info.path, FileSystem::IsDirectory(info)); }, opener);
}

vector<OpenFileInfo> WebHdfsFileSystem::Glob(const string &path, FileOpener *opener) {
	WebHdfsUrl parsed;
	auto client = CreateClient(path, opener, parsed);
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

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "webhdfs_client.hpp"
#include "webhdfs_hadoop_config.hpp"
#include "webhdfs_walk.hpp"

namespace duckdb {

class WebHdfsFileSystem;

class WebHdfsFileHandle : public FileHandle {
public:
	WebHdfsFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, shared_ptr<WebHdfsClient> client,
	                  string hdfs_path);
	//! A writer that was neither closed nor aborted is committed (Close) unless an exception is unwinding or a request
	//! failed; then it is aborted. Errors never escape.
	~WebHdfsFileHandle() override;

	//! Send the rest of a write (the CREATE of an empty or small file included). If that fails, the handle is marked
	//! failed but stays open, so that Abort() can still remove the file.
	void Close() override;
	//! Send buffered writes
	void Flush();
	//! Drop buffered writes and remove a file this handle created (best effort)
	void Abort();
	//! Throw if the handle can't take more writes (closed, or an earlier CREATE/APPEND failed)
	void CheckWritable() const;

	shared_ptr<WebHdfsClient> client;
	string hdfs_path;
	//! File length (readers) or bytes accepted so far (writers)
	idx_t length = 0;
	int64_t modification_time_ms = 0;
	//! The HDFS inode id (fileId; -1 if the server sent none): it changes when a file is replaced
	int64_t file_id = -1;
	//! Stream position for Read/Write without a location
	idx_t position = 0;

	// reading
	mutex buffer_lock;
	vector<data_t> read_buffer;
	idx_t read_buffer_offset = 0;
	idx_t read_buffer_length = 0;

	// writing
	bool writing = false;
	//! The file exists on the server (created by the first flush, or pre-existing when appending): later chunks
	//! are APPENDed
	bool created = false;
	//! Abort() deletes the file: this handle created it, or sent a CREATE that replaces it
	bool delete_on_abort = false;
	bool append = false;
	//! CREATE replaces an existing file; false for ExclusiveCreate / ReturnNullIfExists, so HDFS enforces it
	bool overwrite = true;
	bool closed = false;
	//! A CREATE or APPEND failed. The server may have applied part of it, so nothing is re-sent: later writes and
	//! Close() throw.
	bool failed = false;
	vector<data_t> write_buffer;
	//! Bytes sent to the server
	idx_t written = 0;

private:
	//! Send `write_buffer` as one CREATE or APPEND; marks the handle failed if that throws
	void SendBuffer();
};

class WebHdfsFileSystem : public FileSystem {
public:
	explicit WebHdfsFileSystem(DatabaseInstance &db);
	~WebHdfsFileSystem() override;

	//! A client for the authority of `url` (parses `url` into `parsed`). Throws IOException for invalid URLs, and
	//! PermissionException if the access settings refuse `url`, checked as `access_type` (FILE_TYPE_DIR for calls on
	//! a directory, which allowed_directories matches with a trailing '/', as core does)
	shared_ptr<WebHdfsClient> CreateClient(const string &url, optional_ptr<FileOpener> opener,
	                                       webhdfs::WebHdfsUrl &parsed, FileType access_type);
	WebHdfsEndpoint &GetEndpoint(const string &key);
	vector<WebHdfsEndpoint::Snapshot> EndpointSnapshots();
	//! Resize the small-file cache of every cluster, in use or created later (webhdfs_small_file_cache_size). The
	//! setting's OnSet hook is the only caller: a client never sets the size itself
	void SetSmallFileCacheSize(idx_t bytes);
	//! The size at extension load, unless a SET from another connection already reached the hook (which runs before
	//! the value is stored, so a value read at load could be older than the one it applied)
	void SeedSmallFileCacheSize(idx_t bytes);
	//! The configuration a request to the root of endpoint `key` (scheme://authority) would use for the caller of
	//! `opener`; false if access is refused or it can't be resolved (e.g. an invalid secret)
	bool TryResolveConfig(const string &key, optional_ptr<FileOpener> opener, WebHdfsConfig &out);
	//! hdfs-site.xml + core-site.xml from HADOOP_CONF_DIR / HADOOP_HOME (loaded once)
	const webhdfs::HadoopConfig &SiteConfig();
	DatabaseInstance &GetDatabase() {
		return db;
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	void Reset(FileHandle &handle) override;
	void FileSync(FileHandle &handle) override;
	void AbortFileWrite(FileHandle &handle) override;
	void Truncate(FileHandle &handle, int64_t new_size) override;

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	bool CanHandleFile(const string &fpath) override;
	//! The URL with '.' and '..' resolved as ParseWebHdfsUrl does, so allowed_directories checks the path we request
	string CanonicalizePath(const string &path, optional_ptr<FileOpener> opener = nullptr) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}
	bool IsPathAbsolute(const string &path) override {
		return true;
	}
	string PathSeparator(const string &path) override {
		return "/";
	}
	std::string GetName() const override {
		return "WebHdfsFileSystem";
	}

	//! The OpenFileInfo DuckDB gets for a listed file: size, mtime, type and etag travel with it
	static OpenFileInfo MakeOpenFileInfo(const string &url, const webhdfs::FileStatus &status);

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;
	bool SupportsOpenFileExtended() const override {
		return true;
	}
	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;
	bool SupportsListFilesExtended() const override {
		return true;
	}
	//! A lazily expanded list: readers can start on the first files while the walk continues
	unique_ptr<MultiFileList> GlobFilesExtended(const string &path, const FileGlobInput &input,
	                                            optional_ptr<FileOpener> opener) override;
	bool SupportsGlobExtended() const override {
		return true;
	}

private:
	//! disabled_filesystems and the file access settings, checked before any secret lookup, Kerberos or request
	void CheckAccess(const webhdfs::WebHdfsUrl &parsed, FileType access_type);
	//! The cluster names offered for "scheme://": secret scopes, hdfs-site.xml nameservices and clusters in use
	void ListClusterNames(const string &scheme, optional_ptr<FileOpener> opener,
	                      const std::function<void(OpenFileInfo &info)> &callback);

	DatabaseInstance &db;
	mutex endpoints_lock;
	unordered_map<string, unique_ptr<WebHdfsEndpoint>> endpoints;
	//! The small-file cache size every endpoint uses (guarded by endpoints_lock)
	idx_t small_file_cache_size = WebHdfsSettings::DEFAULT_SMALL_FILE_CACHE_SIZE;
	//! SetSmallFileCacheSize has run (guarded by endpoints_lock)
	bool small_file_cache_size_set = false;
	mutex site_lock;
	bool site_loaded = false;
	webhdfs::HadoopConfig site;
};

} // namespace duckdb

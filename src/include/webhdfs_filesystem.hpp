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
	~WebHdfsFileHandle() override;

	void Close() override;
	//! Send buffered writes
	void Flush();
	//! Drop buffered writes and remove a file this handle created
	void Abort();

	shared_ptr<WebHdfsClient> client;
	string hdfs_path;
	//! File length (readers) or bytes accepted so far (writers)
	idx_t length = 0;
	int64_t modification_time_ms = 0;
	//! Stream position for Read/Write without a location
	idx_t position = 0;

	// reading
	mutex buffer_lock;
	vector<data_t> read_buffer;
	idx_t read_buffer_offset = 0;
	idx_t read_buffer_length = 0;

	// writing
	bool writing = false;
	//! The file exists on the server (created by the first flush, or pre-existing when appending)
	bool created = false;
	bool append = false;
	bool overwrite = true;
	bool closed = false;
	vector<data_t> write_buffer;
	//! Bytes sent to the server
	idx_t written = 0;
};

class WebHdfsFileSystem : public FileSystem {
public:
	explicit WebHdfsFileSystem(DatabaseInstance &db);
	~WebHdfsFileSystem() override;

	//! A client for the authority of `url` (parses `url` into `parsed`). Throws IOException for invalid URLs.
	shared_ptr<WebHdfsClient> CreateClient(const string &url, optional_ptr<FileOpener> opener,
	                                       webhdfs::WebHdfsUrl &parsed);
	WebHdfsEndpoint &GetEndpoint(const string &key);
	vector<WebHdfsEndpoint::Snapshot> EndpointSnapshots();
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

	//! The OpenFileInfo DuckDB gets for a listed file: size, mtime and type travel with it
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
	DatabaseInstance &db;
	mutex endpoints_lock;
	unordered_map<string, unique_ptr<WebHdfsEndpoint>> endpoints;
	mutex site_lock;
	bool site_loaded = false;
	webhdfs::HadoopConfig site;
};

} // namespace duckdb

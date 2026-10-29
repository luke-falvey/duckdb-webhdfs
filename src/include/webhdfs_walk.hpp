#pragma once

// A glob walk over a WebHDFS tree. Entries come out in a stable depth-first order (children sorted by name), while
// up to `parallelism` directory listings are fetched ahead of the walk on worker threads.

#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "webhdfs_client.hpp"
#include "webhdfs_glob.hpp"

#include <condition_variable>
#include <thread>

namespace duckdb {

struct WebHdfsWalkEntry {
	//! Absolute HDFS path (no scheme/authority)
	string path;
	webhdfs::FileStatus status;
};

//! Prunes Hive partition directories during a walk. Paths are absolute HDFS paths.
class WebHdfsWalkPruner {
public:
	virtual ~WebHdfsWalkPruner() = default;
	//! False if no file below `dir_path` can pass the filters (judged from its key=value components)
	virtual bool KeepDirectory(const string &dir_path) = 0;
	//! False if the file can't pass the filters
	virtual bool KeepFile(const string &file_path) = 0;
	//! The only directory names ("value" in key=value, Hive-escaped) that can pass the filters for `key`, or false
	//! if the filters don't pin `key` to a list of values
	virtual bool CandidateValues(const string &key, vector<string> &values) = 0;
};

struct WebHdfsWalkOptions {
	bool include_hidden = false;
	bool skip_permission_errors = false;
	//! Emit regular files only (DuckDB globs); otherwise directories that match are emitted too (webhdfs_ls)
	bool files_only = true;
	idx_t parallelism = 1;
};

struct WebHdfsWalkStats {
	idx_t listings = 0;
	idx_t probes = 0;
	idx_t pruned_directories = 0;
};

class WebHdfsWalk {
public:
	//! `pattern` is the HDFS path (may contain wildcards)
	WebHdfsWalk(shared_ptr<WebHdfsClient> client, const string &pattern, WebHdfsWalkOptions options,
	            shared_ptr<WebHdfsWalkPruner> pruner = nullptr);
	~WebHdfsWalk();

	//! Append up to `max_entries` entries to `out`. Returns false when the walk is exhausted and nothing was added.
	bool Next(vector<WebHdfsWalkEntry> &out, idx_t max_entries);
	//! Walk everything
	vector<WebHdfsWalkEntry> All();

	WebHdfsWalkStats Stats();

private:
	struct Listing {
		bool started = false;
		bool done = false;
		vector<webhdfs::FileStatus> entries;
		std::exception_ptr error;
		bool not_found = false;
		bool permission_denied = false;
	};
	struct Item {
		//! true: emit `entry`; false: walk directory `entry.path` with `states`
		bool emit = false;
		WebHdfsWalkEntry entry;
		std::vector<webhdfs::GlobState> states;
		//! a guessed partition directory that may not exist
		bool probe = false;
		bool is_root = false;
		shared_ptr<Listing> listing;
	};

	void Start();
	void WorkerLoop();
	void Fetch(const string &path, Listing &listing);
	//! Expand a directory item whose listing is complete; pushes child items. Called with `lock` held.
	void Expand(Item &item);
	//! Try to replace the listing of `item` by guessed partition directories; returns true if it did.
	bool TryPartitionShortcut(Item &item);

private:
	shared_ptr<WebHdfsClient> client;
	webhdfs::GlobPattern pattern;
	WebHdfsWalkOptions options;
	shared_ptr<WebHdfsWalkPruner> pruner;

	mutex lock;
	std::condition_variable cv;
	vector<Item> stack;
	bool started = false;
	bool shutdown = false;
	vector<std::thread> workers;
	WebHdfsWalkStats stats;
};

} // namespace duckdb

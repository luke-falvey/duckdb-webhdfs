#pragma once

// A glob walk over a WebHDFS tree. Entries come out in a stable depth-first order (children sorted by name), while
// up to `parallelism` directory listings are fetched ahead of the walk on worker threads. Directories the pattern
// spells out (e.g. `month={5,6}` in `.../year=2024/month={5,6}/*.parquet`) are visited without listing their parent.

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

struct WebHdfsWalkOptions {
	bool include_hidden = false;
	bool skip_permission_errors = false;
	//! Emit regular files only (DuckDB globs); otherwise directories that match are emitted too (webhdfs_ls)
	bool files_only = true;
	idx_t parallelism = 1;
};

struct WebHdfsWalkStats {
	//! Directories listed
	idx_t listings = 0;
	//! Directories visited by name without listing their parent
	idx_t probes = 0;
};

class WebHdfsWalk {
public:
	//! `pattern` is the HDFS path (may contain wildcards)
	WebHdfsWalk(shared_ptr<WebHdfsClient> client, const string &pattern, WebHdfsWalkOptions options);
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
		//! the listing was replaced by the directory names the pattern spells out
		bool probe = false;
		bool is_root = false;
		shared_ptr<Listing> listing;
	};

	void Start();
	void WorkerLoop();
	void Fetch(const string &path, Listing &listing);
	//! Expand a directory item whose listing is complete; pushes child items. Called with `lock` held.
	void Expand(Item &item);
	//! When the pattern names the children of `item` literally, replace its listing with those names
	bool TryLiteralShortcut(Item &item);

private:
	shared_ptr<WebHdfsClient> client;
	webhdfs::GlobPattern pattern;
	WebHdfsWalkOptions options;

	mutex lock;
	std::condition_variable cv;
	vector<Item> stack;
	bool started = false;
	bool shutdown = false;
	vector<std::thread> workers;
	WebHdfsWalkStats stats;
};

} // namespace duckdb

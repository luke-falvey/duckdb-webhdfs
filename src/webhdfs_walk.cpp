#include "webhdfs_walk.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>

namespace duckdb {

using webhdfs::FileStatus;

static constexpr idx_t PREFETCH_WINDOW_PER_THREAD = 4;

WebHdfsWalk::WebHdfsWalk(shared_ptr<WebHdfsClient> client_p, const string &path_pattern, WebHdfsWalkOptions options_p,
                         shared_ptr<WebHdfsWalkPruner> pruner_p)
    : client(std::move(client_p)), options(options_p), pruner(std::move(pruner_p)) {
	try {
		pattern = webhdfs::GlobPattern::Compile(path_pattern, options.include_hidden);
	} catch (webhdfs::GlobError &e) {
		throw InvalidInputException("Invalid WebHDFS glob pattern \"%s\": %s", client->UrlFor(path_pattern),
		                            e.what());
	}
}

WebHdfsWalk::~WebHdfsWalk() {
	{
		lock_guard<mutex> guard(lock);
		shutdown = true;
	}
	cv.notify_all();
	for (auto &worker : workers) {
		worker.join();
	}
}

WebHdfsWalkStats WebHdfsWalk::Stats() {
	lock_guard<mutex> guard(lock);
	return stats;
}

void WebHdfsWalk::Start() {
	started = true;
	auto &root = pattern.Root();
	if (pattern.IsLiteral() || pattern.MatchesRoot()) {
		FileStatus status;
		bool exists = client->TryGetFileStatus(root, status);
		if (exists) {
			if (pattern.IsLiteral() && status.is_dir && !options.files_only) {
				// a plain directory: list its children
				Item item;
				item.entry.path = root;
				item.entry.status = status;
				item.is_root = true;
				item.listing = make_shared_ptr<Listing>();
				// every visible child matches, nothing below it
				stack.push_back(std::move(item));
				return;
			}
			if (!status.is_dir || !options.files_only) {
				Item emit;
				emit.emit = true;
				emit.entry.path = root;
				emit.entry.status = status;
				if (pattern.IsLiteral()) {
					stack.push_back(std::move(emit));
					return;
				}
				// the root also matches: emit it after its subtree (pushed first, popped last)
				stack.push_back(std::move(emit));
			}
		}
		if (pattern.IsLiteral()) {
			return;
		}
	}
	Item item;
	item.entry.path = root;
	item.states = pattern.Start();
	item.is_root = true;
	item.listing = make_shared_ptr<Listing>();
	stack.push_back(std::move(item));
	auto threads = options.parallelism > 1 ? options.parallelism - 1 : 0;
	for (idx_t i = 0; i < threads; i++) {
		workers.emplace_back([this]() { WorkerLoop(); });
	}
}

void WebHdfsWalk::Fetch(const string &path, Listing &listing) {
	vector<FileStatus> entries;
	std::exception_ptr error;
	bool not_found = false;
	bool permission_denied = false;
	try {
		client->ListStatus(path, [&](vector<FileStatus> &page) {
			for (auto &entry : page) {
				entries.push_back(std::move(entry));
			}
		});
	} catch (IOException &e) {
		// a directory that vanished (or a guessed partition that doesn't exist) is just empty
		if (StringUtil::Contains(e.what(), "no such file or directory")) {
			not_found = true;
		} else {
			error = std::current_exception();
		}
	} catch (PermissionException &e) {
		permission_denied = true;
		error = std::current_exception();
	} catch (...) {
		error = std::current_exception();
	}
	lock_guard<mutex> guard(lock);
	listing.entries = std::move(entries);
	listing.error = error;
	listing.not_found = not_found;
	listing.permission_denied = permission_denied;
	listing.done = true;
	stats.listings++;
}

void WebHdfsWalk::WorkerLoop() {
	unique_lock<mutex> guard(lock);
	while (!shutdown) {
		shared_ptr<Listing> target;
		string path;
		bool rescan = false;
		auto window = MinValue<idx_t>(stack.size(), options.parallelism * PREFETCH_WINDOW_PER_THREAD);
		for (idx_t i = 0; i < window; i++) {
			auto &item = stack[stack.size() - 1 - i];
			if (!item.emit && item.listing && !item.listing->started) {
				if (TryPartitionShortcut(item)) {
					// replaced by guessed partition directories: nothing to fetch for this one
					rescan = true;
					break;
				}
				target = item.listing;
				path = item.entry.path;
				break;
			}
		}
		if (rescan) {
			cv.notify_all();
			continue;
		}
		if (!target) {
			cv.wait(guard);
			continue;
		}
		target->started = true;
		guard.unlock();
		Fetch(path, *target);
		cv.notify_all();
		guard.lock();
	}
}

bool WebHdfsWalk::TryPartitionShortcut(Item &item) {
	if (!pruner || item.states.empty()) {
		return false;
	}
	string key;
	if (!pattern.PartitionComponentKey(item.states, key)) {
		return false;
	}
	vector<string> values;
	if (!pruner->CandidateValues(key, values)) {
		return false;
	}
	// Mark the listing as done with synthesized directory entries; they are probed when walked.
	item.listing->started = true;
	item.listing->done = true;
	item.probe = true;
	item.listing->entries.clear();
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	for (auto &value : values) {
		FileStatus status;
		status.path_suffix = key + "=" + value;
		status.is_dir = true;
		item.listing->entries.push_back(std::move(status));
	}
	return true;
}

void WebHdfsWalk::Expand(Item &item) {
	auto &listing = *item.listing;
	if (listing.not_found) {
		// globbing below a missing directory (or a guessed partition that doesn't exist) matches nothing
		return;
	}
	if (listing.error) {
		if (listing.permission_denied && options.skip_permission_errors && !item.is_root) {
			return;
		}
		std::rethrow_exception(listing.error);
	}
	auto entries = std::move(listing.entries);
	listing.entries.clear();
	std::sort(entries.begin(), entries.end(),
	          [](const FileStatus &a, const FileStatus &b) { return a.path_suffix < b.path_suffix; });
	bool synthesized = item.probe;
	bool list_directory = pattern.IsLiteral();
	// push in reverse so the smallest name is walked first
	for (idx_t i = entries.size(); i > 0; i--) {
		auto &status = entries[i - 1];
		if (status.path_suffix.empty()) {
			// LISTSTATUS on a file returns the file itself
			continue;
		}
		auto child_path = webhdfs::JoinPath(item.entry.path, status.path_suffix);
		webhdfs::GlobMatch match;
		if (list_directory) {
			// listing a plain directory: every visible child, no recursion
			if (!options.include_hidden && webhdfs::IsHiddenName(status.path_suffix)) {
				continue;
			}
			match.emit = true;
		} else {
			match = pattern.Match(item.states, status.path_suffix, status.is_dir);
		}
		if (status.is_dir && !match.next.empty()) {
			if (pruner && !pruner->KeepDirectory(child_path)) {
				stats.pruned_directories++;
			} else {
				Item child;
				child.entry.path = child_path;
				child.entry.status = status;
				child.states = std::move(match.next);
				child.listing = make_shared_ptr<Listing>();
				if (synthesized) {
					stats.probes++;
				}
				stack.push_back(std::move(child));
			}
		}
		if (match.emit && !synthesized) {
			if (status.is_dir && options.files_only) {
				continue;
			}
			if (!status.is_dir && pruner && !pruner->KeepFile(child_path)) {
				continue;
			}
			Item emit;
			emit.emit = true;
			emit.entry.path = child_path;
			emit.entry.status = std::move(status);
			stack.push_back(std::move(emit));
		}
	}
}

bool WebHdfsWalk::Next(vector<WebHdfsWalkEntry> &out, idx_t max_entries) {
	unique_lock<mutex> guard(lock);
	if (!started) {
		guard.unlock();
		{
			// Start() makes requests; run it without holding the lock (no workers exist yet)
			Start();
		}
		guard.lock();
		cv.notify_all();
	}
	idx_t added = 0;
	while (added < max_entries && !stack.empty()) {
		auto &top = stack.back();
		if (top.emit) {
			out.push_back(std::move(top.entry));
			stack.pop_back();
			added++;
			continue;
		}
		auto listing = top.listing;
		if (!listing->started) {
			// may replace the listing by guessed partition directories
			TryPartitionShortcut(top);
		}
		if (!listing->done) {
			if (added > 0) {
				// hand out what we have rather than wait for the network
				break;
			}
			if (!listing->started) {
				listing->started = true;
				auto path = top.entry.path;
				guard.unlock();
				Fetch(path, *listing);
				guard.lock();
				cv.notify_all();
			} else {
				cv.wait(guard, [&]() { return listing->done; });
			}
			continue;
		}
		Item item = std::move(stack.back());
		stack.pop_back();
		Expand(item);
		cv.notify_all();
	}
	return added > 0;
}

vector<WebHdfsWalkEntry> WebHdfsWalk::All() {
	vector<WebHdfsWalkEntry> result;
	while (Next(result, STANDARD_VECTOR_SIZE)) {
	}
	return result;
}

} // namespace duckdb

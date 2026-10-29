#pragma once

#include "duckdb.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "webhdfs_walk.hpp"

namespace duckdb {

class WebHdfsFileSystem;

//! The files matching WebHDFS globs, expanded lazily as a reader asks for them: a scan starts on the first files
//! while the rest of the tree is still being listed.
class WebHdfsFileList : public LazyMultiFileList {
public:
	WebHdfsFileList(WebHdfsFileSystem &fs, optional_ptr<ClientContext> context, optional_ptr<FileOpener> opener,
	                vector<string> globs);

	vector<OpenFileInfo> GetDisplayFileList(optional_idx max_files = optional_idx()) const override;
	unique_ptr<MultiFileList> Copy() const override;

protected:
	bool ExpandNextPath() const override;

private:
	WebHdfsFileSystem &fs;
	optional_ptr<FileOpener> opener;
	vector<string> globs;

	mutable idx_t current_glob = 0;
	mutable shared_ptr<WebHdfsClient> client;
	mutable unique_ptr<WebHdfsWalk> walk;
	mutable string prefix;
};

} // namespace duckdb

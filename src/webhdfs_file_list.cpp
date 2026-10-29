#include "webhdfs_file_list.hpp"

#include "webhdfs_filesystem.hpp"
#include "webhdfs_glob.hpp"

namespace duckdb {

WebHdfsFileList::WebHdfsFileList(WebHdfsFileSystem &fs_p, optional_ptr<ClientContext> context_p,
                                 optional_ptr<FileOpener> opener_p, vector<string> globs_p)
    : LazyMultiFileList(context_p), fs(fs_p), opener(opener_p), globs(std::move(globs_p)) {
}

bool WebHdfsFileList::ExpandNextPath() const {
	while (true) {
		if (!walk) {
			if (current_glob >= globs.size()) {
				return false;
			}
			webhdfs::WebHdfsUrl parsed;
			client = fs.CreateClient(globs[current_glob], opener, parsed);
			prefix = parsed.Prefix();
			if (!webhdfs::HasGlobCharacters(parsed.path)) {
				current_glob++;
				webhdfs::FileStatus status;
				if (client->TryGetFileStatus(parsed.path, status) && !status.is_dir) {
					expanded_files.push_back(WebHdfsFileSystem::MakeOpenFileInfo(parsed.ToString(), status));
					return true;
				}
				continue;
			}
			auto &settings = client->Settings();
			WebHdfsWalkOptions options;
			options.include_hidden = settings.include_hidden;
			options.skip_permission_errors = settings.skip_permission_errors;
			options.parallelism = settings.list_parallelism;
			options.files_only = true;
			walk = make_uniq<WebHdfsWalk>(client, parsed.path, options);
		}
		vector<WebHdfsWalkEntry> entries;
		if (!walk->Next(entries, STANDARD_VECTOR_SIZE)) {
			walk.reset();
			current_glob++;
			continue;
		}
		for (auto &entry : entries) {
			expanded_files.push_back(WebHdfsFileSystem::MakeOpenFileInfo(prefix + entry.path, entry.status));
		}
		return true;
	}
}

vector<OpenFileInfo> WebHdfsFileList::GetDisplayFileList(optional_idx max_files) const {
	vector<OpenFileInfo> result;
	for (auto &glob : globs) {
		if (max_files.IsValid() && result.size() >= max_files.GetIndex()) {
			break;
		}
		result.emplace_back(glob);
	}
	return result;
}

unique_ptr<MultiFileList> WebHdfsFileList::Copy() const {
	return make_uniq<WebHdfsFileList>(fs, context, opener, globs);
}

} // namespace duckdb

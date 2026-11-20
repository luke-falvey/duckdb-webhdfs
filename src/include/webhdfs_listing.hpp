#pragma once

// Paging checks for LISTSTATUS_BATCH. Header-only and free of DuckDB types, so the unit tests can use it.

#include <cstdint>
#include <string>

namespace webhdfs {

//! LISTSTATUS_BATCH pages continue after the last name of the previous page (`startAfter`). A page after the first
//! one must therefore hold entries while more remain, and its last name must differ from the previous `startAfter`;
//! otherwise the next request would repeat the same page forever. The NameNode orders names by signed byte value
//! (DFSUtilClient.compareBytes), so this deliberately checks only for progress, not for an ordering.
//! Returns false and sets `error` when a page does not advance.
inline bool ListingPageAdvances(bool first_page, const std::string &previous_start_after, size_t page_entries,
                                const std::string &page_last_name, int64_t remaining, std::string &error) {
	if (page_entries == 0) {
		if (!first_page && remaining > 0) {
			error = "the server returned an empty page with " + std::to_string(remaining) + " entries remaining";
			return false;
		}
		return true;
	}
	if (!first_page && page_last_name == previous_start_after) {
		error = "the page ending at \"" + page_last_name + "\" repeats the previous page, which ended at \"" +
		        previous_start_after + "\"";
		return false;
	}
	if (remaining > 0 && page_last_name.empty()) {
		error = "the page's last entry has no name, but " + std::to_string(remaining) + " entries remain";
		return false;
	}
	return true;
}

} // namespace webhdfs

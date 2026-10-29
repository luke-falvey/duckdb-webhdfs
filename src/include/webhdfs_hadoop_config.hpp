#pragma once

// Reads the few hdfs-site.xml keys needed to resolve an HA nameservice to its NameNode HTTP addresses.
// Plain C++ (no DuckDB headers).

#include <map>
#include <string>
#include <vector>

namespace webhdfs {

class HadoopConfig {
public:
	//! Parse Hadoop configuration XML text (<configuration><property><name/><value/></property>...).
	//! Later definitions of a key override earlier ones. Returns false and sets `error` on malformed XML.
	bool ParseXml(const std::string &xml, std::string &error);
	//! Parse a file; a missing file is not an error (returns true, adds nothing).
	bool LoadFile(const std::string &path, std::string &error);

	bool Has(const std::string &key) const {
		return properties.count(key) > 0;
	}
	std::string Get(const std::string &key, const std::string &default_value = "") const;

	//! The HTTP (or HTTPS) addresses of the NameNodes of a nameservice, in configured order:
	//! dfs.ha.namenodes.<ns> + dfs.namenode.http[s]-address.<ns>.<nn>, or dfs.namenode.http[s]-address.<ns> for a
	//! non-HA nameservice. Empty when the nameservice is not configured.
	std::vector<std::string> NameNodeAddresses(const std::string &nameservice, bool https) const;

	//! The directory holding hdfs-site.xml: HADOOP_CONF_DIR, else HADOOP_HOME/etc/hadoop, else "".
	static std::string ConfigDirectory();

	std::map<std::string, std::string> properties;
};

//! Split on commas and trim whitespace, dropping empty items.
std::vector<std::string> SplitCommaList(const std::string &value);

} // namespace webhdfs

#include "webhdfs_hadoop_config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace webhdfs {

static std::string Trim(const std::string &s) {
	size_t start = 0;
	while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
		start++;
	}
	size_t end = s.size();
	while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r')) {
		end--;
	}
	return s.substr(start, end - start);
}

static std::string DecodeEntities(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '&') {
			auto semi = s.find(';', i);
			if (semi != std::string::npos) {
				auto entity = s.substr(i + 1, semi - i - 1);
				if (entity == "amp") {
					out += '&';
				} else if (entity == "lt") {
					out += '<';
				} else if (entity == "gt") {
					out += '>';
				} else if (entity == "quot") {
					out += '"';
				} else if (entity == "apos") {
					out += '\'';
				} else {
					out += s.substr(i, semi - i + 1);
				}
				i = semi;
				continue;
			}
		}
		out += s[i];
	}
	return out;
}

static std::string StripComments(const std::string &xml) {
	std::string out;
	size_t pos = 0;
	while (pos < xml.size()) {
		auto start = xml.find("<!--", pos);
		if (start == std::string::npos) {
			out += xml.substr(pos);
			break;
		}
		out += xml.substr(pos, start - pos);
		auto end = xml.find("-->", start + 4);
		if (end == std::string::npos) {
			break;
		}
		pos = end + 3;
	}
	return out;
}

// Text of the first <tag>...</tag> inside [begin, end), or false if absent.
static bool FindElement(const std::string &xml, size_t begin, size_t end, const std::string &tag,
                        std::string &value) {
	auto open = xml.find("<" + tag, begin);
	while (open != std::string::npos && open < end) {
		auto after = open + tag.size() + 1;
		if (after < xml.size() && (xml[after] == '>' || xml[after] == ' ' || xml[after] == '/')) {
			break;
		}
		open = xml.find("<" + tag, after);
	}
	if (open == std::string::npos || open >= end) {
		return false;
	}
	auto gt = xml.find('>', open);
	if (gt == std::string::npos || gt >= end) {
		return false;
	}
	if (xml[gt - 1] == '/') {
		value.clear();
		return true;
	}
	auto close = xml.find("</" + tag + ">", gt);
	if (close == std::string::npos || close > end) {
		return false;
	}
	value = DecodeEntities(Trim(xml.substr(gt + 1, close - gt - 1)));
	return true;
}

bool HadoopConfig::ParseXml(const std::string &raw, std::string &error) {
	auto xml = StripComments(raw);
	size_t pos = 0;
	while (true) {
		auto start = xml.find("<property", pos);
		if (start == std::string::npos) {
			break;
		}
		auto end = xml.find("</property>", start);
		if (end == std::string::npos) {
			error = "unterminated <property> element";
			return false;
		}
		std::string name;
		std::string value;
		if (FindElement(xml, start, end, "name", name) && !name.empty()) {
			if (!FindElement(xml, start, end, "value", value)) {
				value.clear();
			}
			properties[name] = value;
		}
		pos = end + 11;
	}
	return true;
}

bool HadoopConfig::LoadFile(const std::string &path, std::string &error) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return true;
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	std::string parse_error;
	if (!ParseXml(buffer.str(), parse_error)) {
		error = path + ": " + parse_error;
		return false;
	}
	return true;
}

std::string HadoopConfig::Get(const std::string &key, const std::string &default_value) const {
	auto entry = properties.find(key);
	return entry == properties.end() ? default_value : entry->second;
}

std::vector<std::string> SplitCommaList(const std::string &value) {
	std::vector<std::string> result;
	size_t pos = 0;
	while (pos <= value.size()) {
		auto comma = value.find(',', pos);
		if (comma == std::string::npos) {
			comma = value.size();
		}
		auto item = Trim(value.substr(pos, comma - pos));
		if (!item.empty()) {
			result.push_back(item);
		}
		pos = comma + 1;
	}
	return result;
}

std::vector<std::string> HadoopConfig::NameNodeAddresses(const std::string &nameservice, bool https) const {
	std::vector<std::string> result;
	auto key_base = std::string(https ? "dfs.namenode.https-address." : "dfs.namenode.http-address.") + nameservice;
	auto namenodes = SplitCommaList(Get("dfs.ha.namenodes." + nameservice));
	for (auto &nn : namenodes) {
		auto address = Get(key_base + "." + nn);
		if (!address.empty()) {
			result.push_back(address);
		}
	}
	if (result.empty()) {
		auto address = Get(key_base);
		if (!address.empty()) {
			result.push_back(address);
		}
	}
	return result;
}

std::string HadoopConfig::ConfigDirectory() {
	const char *conf_dir = std::getenv("HADOOP_CONF_DIR");
	if (conf_dir && *conf_dir) {
		return conf_dir;
	}
	const char *home = std::getenv("HADOOP_HOME");
	if (home && *home) {
		return std::string(home) + "/etc/hadoop";
	}
	return "";
}

} // namespace webhdfs

#include "webhdfs_glob.hpp"

#include <algorithm>

namespace webhdfs {

static constexpr size_t MAX_BRACE_EXPANSIONS = 4096;

bool IsHiddenName(const std::string &name) {
	return !name.empty() && (name[0] == '.' || name[0] == '_');
}

static bool HasUnescaped(const std::string &text, const char *chars) {
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\') {
			i++;
			continue;
		}
		for (const char *c = chars; *c; c++) {
			if (text[i] == *c) {
				return true;
			}
		}
	}
	return false;
}

bool HasGlobCharacters(const std::string &text) {
	return HasUnescaped(text, "*?[{");
}

std::string UnescapeGlob(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			i++;
		}
		result += text[i];
	}
	return result;
}

// Position of the '}' that closes the '{' at `open`, or npos.
static size_t FindClosingBrace(const std::string &pattern, size_t open) {
	int depth = 0;
	for (size_t i = open; i < pattern.size(); i++) {
		if (pattern[i] == '\\') {
			i++;
			continue;
		}
		if (pattern[i] == '{') {
			depth++;
		} else if (pattern[i] == '}') {
			depth--;
			if (depth == 0) {
				return i;
			}
		}
	}
	return std::string::npos;
}

static void ExpandInto(const std::string &pattern, std::vector<std::string> &out) {
	size_t open = std::string::npos;
	for (size_t i = 0; i < pattern.size(); i++) {
		if (pattern[i] == '\\') {
			i++;
			continue;
		}
		if (pattern[i] == '{') {
			open = i;
			break;
		}
	}
	if (open == std::string::npos) {
		out.push_back(pattern);
		return;
	}
	auto close = FindClosingBrace(pattern, open);
	if (close == std::string::npos) {
		throw GlobError("unclosed '{' in pattern '" + pattern + "'");
	}
	auto prefix = pattern.substr(0, open);
	auto body = pattern.substr(open + 1, close - open - 1);
	auto suffix = pattern.substr(close + 1);
	// split the body on top-level commas
	std::vector<std::string> options;
	int depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < body.size(); i++) {
		if (body[i] == '\\') {
			i++;
			continue;
		}
		if (body[i] == '{') {
			depth++;
		} else if (body[i] == '}') {
			depth--;
		} else if (body[i] == ',' && depth == 0) {
			options.push_back(body.substr(start, i - start));
			start = i + 1;
		}
	}
	options.push_back(body.substr(start));
	for (auto &option : options) {
		ExpandInto(prefix + option + suffix, out);
		if (out.size() > MAX_BRACE_EXPANSIONS) {
			throw GlobError("pattern expands to more than 4096 alternatives");
		}
	}
}

std::vector<std::string> ExpandBraces(const std::string &pattern) {
	std::vector<std::string> result;
	ExpandInto(pattern, result);
	// drop duplicates, keeping the first occurrence
	std::vector<std::string> unique;
	for (auto &p : result) {
		if (std::find(unique.begin(), unique.end(), p) == unique.end()) {
			unique.push_back(p);
		}
	}
	return unique;
}

static size_t Utf8Length(unsigned char lead) {
	if (lead < 0x80) {
		return 1;
	}
	if ((lead >> 5) == 0x6) {
		return 2;
	}
	if ((lead >> 4) == 0xE) {
		return 3;
	}
	if ((lead >> 3) == 0x1E) {
		return 4;
	}
	return 1;
}

// Match name[n] against the class starting at pattern[p] == '['. Sets `end` past the closing ']'.
// Returns false in `valid` for an unterminated class (which is then matched as a literal '[').
static bool MatchClass(const std::string &pattern, size_t p, char c, size_t &end, bool &valid) {
	size_t i = p + 1;
	bool negate = false;
	if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) {
		negate = true;
		i++;
	}
	bool matched = false;
	bool first = true;
	while (i < pattern.size() && (first || pattern[i] != ']')) {
		first = false;
		char lo = pattern[i];
		if (lo == '\\' && i + 1 < pattern.size()) {
			i++;
			lo = pattern[i];
		}
		char hi = lo;
		if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
			hi = pattern[i + 2];
			if (hi == '\\' && i + 3 < pattern.size()) {
				i++;
				hi = pattern[i + 2];
			}
			i += 2;
		}
		auto uc = static_cast<unsigned char>(c);
		if (uc >= static_cast<unsigned char>(lo) && uc <= static_cast<unsigned char>(hi)) {
			matched = true;
		}
		i++;
	}
	if (i >= pattern.size()) {
		valid = false;
		return false;
	}
	valid = true;
	end = i + 1;
	return matched != negate;
}

bool MatchComponent(const std::string &name, const std::string &pattern) {
	size_t p = 0;
	size_t n = 0;
	size_t star_p = std::string::npos;
	size_t star_n = 0;
	while (n < name.size()) {
		if (p < pattern.size()) {
			char pc = pattern[p];
			if (pc == '*') {
				star_p = p++;
				star_n = n;
				continue;
			}
			if (pc == '?') {
				p++;
				n += Utf8Length(static_cast<unsigned char>(name[n]));
				continue;
			}
			if (pc == '[') {
				size_t end = 0;
				bool valid = false;
				bool matched = MatchClass(pattern, p, name[n], end, valid);
				if (valid) {
					if (matched) {
						p = end;
						n++;
						continue;
					}
				} else if (name[n] == '[') {
					p++;
					n++;
					continue;
				}
			} else if (pc == '\\' && p + 1 < pattern.size()) {
				if (name[n] == pattern[p + 1]) {
					p += 2;
					n++;
					continue;
				}
			} else if (pc == name[n]) {
				p++;
				n++;
				continue;
			}
		}
		if (star_p == std::string::npos) {
			return false;
		}
		p = star_p + 1;
		n = ++star_n;
	}
	while (p < pattern.size() && pattern[p] == '*') {
		p++;
	}
	return p == pattern.size();
}

static std::vector<std::string> SplitComponents(const std::string &path) {
	std::vector<std::string> result;
	size_t pos = 0;
	while (pos <= path.size()) {
		auto next = path.find('/', pos);
		if (next == std::string::npos) {
			next = path.size();
		}
		auto part = path.substr(pos, next - pos);
		if (!part.empty() && part != ".") {
			result.push_back(std::move(part));
		}
		pos = next + 1;
	}
	return result;
}

GlobPattern GlobPattern::Compile(const std::string &pattern, bool include_hidden) {
	if (pattern.empty() || pattern[0] != '/') {
		throw GlobError("glob patterns must be absolute paths");
	}
	GlobPattern result;
	result.include_hidden = include_hidden;
	auto expanded = ExpandBraces(pattern);

	// Split every alternative into components; record how many leading components are literal.
	std::vector<std::vector<std::string>> parts;
	std::vector<size_t> literal_prefix;
	for (auto &alt : expanded) {
		auto components = SplitComponents(alt);
		size_t literal_count = 0;
		while (literal_count < components.size() && !HasUnescaped(components[literal_count], "*?[")) {
			literal_count++;
		}
		parts.push_back(std::move(components));
		literal_prefix.push_back(literal_count);
	}

	// The root is the longest literal directory prefix shared by all alternatives.
	size_t root_len = literal_prefix[0];
	for (size_t a = 0; a < parts.size(); a++) {
		root_len = std::min(root_len, literal_prefix[a]);
		for (size_t i = 0; i < root_len; i++) {
			if (UnescapeGlob(parts[a][i]) != UnescapeGlob(parts[0][i])) {
				root_len = i;
				break;
			}
		}
	}
	bool all_literal = parts.size() == 1 && literal_prefix[0] == parts[0].size();
	std::string root;
	for (size_t i = 0; i < root_len; i++) {
		root += "/" + UnescapeGlob(parts[0][i]);
	}
	result.root = root.empty() ? "/" : root;
	result.literal = all_literal;

	for (size_t a = 0; a < parts.size(); a++) {
		std::vector<Component> components;
		for (size_t i = root_len; i < parts[a].size(); i++) {
			auto &text = parts[a][i];
			Component component;
			if (text == "**") {
				component.kind = Kind::RECURSIVE;
			} else if (HasUnescaped(text, "*?[")) {
				component.kind = Kind::PATTERN;
				component.text = text;
				char first = text[0] == '\\' && text.size() > 1 ? text[1] : text[0];
				component.explicit_hidden = first == '.' || first == '_';
			} else {
				component.kind = Kind::LITERAL;
				component.text = UnescapeGlob(text);
				component.explicit_hidden = IsHiddenName(component.text);
			}
			components.push_back(std::move(component));
		}
		if (components.empty()) {
			result.matches_root = true;
		}
		result.alternatives.push_back(std::move(components));
	}
	if (!result.literal) {
		for (uint32_t a = 0; a < result.alternatives.size(); a++) {
			if (!result.alternatives[a].empty()) {
				result.start.push_back(GlobState {a, 0});
			}
		}
	}
	return result;
}

void GlobPattern::Step(const GlobState &state, const std::string &name, bool is_dir, bool hidden,
                       GlobMatch &result) const {
	auto &components = alternatives[state.alternative];
	if (state.component >= components.size()) {
		return;
	}
	auto &component = components[state.component];
	bool last = state.component + 1 == components.size();
	if (component.kind == Kind::RECURSIVE) {
		bool visible = include_hidden || !hidden;
		if (last) {
			// trailing `**`: everything below, at any depth
			if (visible) {
				result.emit = true;
			}
		} else {
			// `**` matching zero levels: try the next component against this child
			Step(GlobState {state.alternative, state.component + 1}, name, is_dir, hidden, result);
		}
		if (is_dir && visible) {
			result.next.push_back(state);
		}
		return;
	}
	bool matched = component.kind == Kind::LITERAL ? name == component.text : MatchComponent(name, component.text);
	if (!matched) {
		return;
	}
	if (hidden && !include_hidden && !component.explicit_hidden) {
		return;
	}
	if (last) {
		result.emit = true;
	} else if (is_dir) {
		result.next.push_back(GlobState {state.alternative, state.component + 1});
	}
}

GlobMatch GlobPattern::Match(const std::vector<GlobState> &states, const std::string &name, bool is_dir) const {
	GlobMatch result;
	bool hidden = IsHiddenName(name);
	for (auto &state : states) {
		Step(state, name, is_dir, hidden, result);
	}
	if (result.next.size() > 1) {
		std::sort(result.next.begin(), result.next.end());
		result.next.erase(std::unique(result.next.begin(), result.next.end()), result.next.end());
	}
	return result;
}

bool GlobPattern::PartitionComponentKey(const std::vector<GlobState> &states, std::string &key) const {
	if (states.empty()) {
		return false;
	}
	std::string found;
	for (auto &state : states) {
		auto &components = alternatives[state.alternative];
		if (state.component + 1 >= components.size()) {
			// last component (or past it): candidates could be files, so names can't be guessed
			return false;
		}
		auto &component = components[state.component];
		if (component.kind == Kind::RECURSIVE) {
			return false;
		}
		std::string raw = component.text;
		size_t eq = std::string::npos;
		for (size_t i = 0; i < raw.size(); i++) {
			if (raw[i] == '\\') {
				i++;
				continue;
			}
			if (raw[i] == '=') {
				eq = i;
				break;
			}
		}
		if (eq == std::string::npos || eq == 0) {
			return false;
		}
		auto prefix = raw.substr(0, eq);
		if (component.kind == Kind::PATTERN && HasUnescaped(prefix, "*?[")) {
			return false;
		}
		auto this_key = component.kind == Kind::PATTERN ? UnescapeGlob(prefix) : prefix;
		if (found.empty()) {
			found = this_key;
		} else if (found != this_key) {
			return false;
		}
	}
	key = found;
	return true;
}

} // namespace webhdfs

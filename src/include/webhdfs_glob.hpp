#pragma once

// Glob patterns for WebHDFS directory walks. Plain C++ (no DuckDB headers).
//
// Syntax: `*`, `?` (one UTF-8 character), `[abc]`, `[a-z]`, `[!abc]` / `[^abc]`, `\` escapes, `**` as a whole
// path component (any number of directory levels), and `{a,b}` alternation (nestable).
//
// Hidden entries (names starting with `.` or `_`, e.g. `_SUCCESS`, `_temporary`, `.part.crc`) are skipped unless
// the pattern is compiled with `include_hidden`, or the component that matches them names the hidden character
// explicitly (a literal like `_SUCCESS`, or a pattern starting with `_` / `.`). `**` never descends into hidden
// directories.
//
// A pattern compiles into a set of alternatives (one per brace expansion), each a list of components below a common
// literal root directory. The walk keeps a set of GlobStates per directory and calls Match() for every child to
// learn whether the child is a match and which states to descend into it with.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace webhdfs {

class GlobError : public std::runtime_error {
public:
	explicit GlobError(const std::string &message) : std::runtime_error(message) {
	}
};

struct GlobState {
	uint32_t alternative = 0;
	uint32_t component = 0;

	bool operator==(const GlobState &other) const {
		return alternative == other.alternative && component == other.component;
	}
	bool operator<(const GlobState &other) const {
		return alternative < other.alternative || (alternative == other.alternative && component < other.component);
	}
};

struct GlobMatch {
	//! The child itself matches the pattern
	bool emit = false;
	//! States to walk the child with when it is a directory (empty: don't descend)
	std::vector<GlobState> next;
};

class GlobPattern {
public:
	//! Compile an absolute path pattern (no scheme/authority). Throws GlobError for invalid patterns.
	static GlobPattern Compile(const std::string &pattern, bool include_hidden);

	//! The literal directory the walk starts from.
	const std::string &Root() const {
		return root;
	}
	//! True when the pattern names exactly one path with no wildcards (Root() is that path).
	bool IsLiteral() const {
		return literal;
	}
	//! True when some alternative names the root itself (e.g. "{/a,/a/b}").
	bool MatchesRoot() const {
		return matches_root;
	}
	//! States for the children of Root().
	const std::vector<GlobState> &Start() const {
		return start;
	}
	//! Match one child of a directory that is being walked with `states`.
	GlobMatch Match(const std::vector<GlobState> &states, const std::string &name, bool is_dir) const;

	//! If every state is at a non-final component of the form `key=<anything>` with a literal key shared by all
	//! states, return true and the key. Used to try partition directories by name instead of listing the parent.
	bool PartitionComponentKey(const std::vector<GlobState> &states, std::string &key) const;

private:
	enum class Kind { LITERAL, PATTERN, RECURSIVE };
	struct Component {
		Kind kind;
		//! LITERAL: unescaped name. PATTERN: raw pattern text.
		std::string text;
		//! The component names a hidden entry on purpose (literal or pattern starting with `.` / `_`)
		bool explicit_hidden = false;
	};

	void Step(const GlobState &state, const std::string &name, bool is_dir, bool hidden, GlobMatch &result) const;

private:
	std::vector<std::vector<Component>> alternatives;
	std::string root;
	std::vector<GlobState> start;
	bool literal = false;
	bool matches_root = false;
	bool include_hidden = false;
};

//! Hadoop-style hidden name: starts with '.' or '_'.
bool IsHiddenName(const std::string &name);
//! True if the text contains an unescaped `*`, `?`, `[` or `{`.
bool HasGlobCharacters(const std::string &text);
//! Expand `{a,b}` groups into plain patterns. Throws GlobError for unclosed groups or too many expansions.
std::vector<std::string> ExpandBraces(const std::string &pattern);
//! Match one path component against a pattern (`*`, `?`, `[...]`, `\`). Brace groups must already be expanded.
bool MatchComponent(const std::string &name, const std::string &pattern);
//! Remove `\` escapes.
std::string UnescapeGlob(const std::string &text);

} // namespace webhdfs

// Cluster-free unit tests: URL handling, glob matching, Hadoop config parsing and WebHDFS JSON parsing.

#include "test_main.hpp"
#include "webhdfs_glob.hpp"
#include "webhdfs_hadoop_config.hpp"
#include "webhdfs_json.hpp"
#include "webhdfs_listing.hpp"
#include "webhdfs_url.hpp"

#include <algorithm>
#include <map>
#include <set>

using namespace webhdfs; // NOLINT

//===--------------------------------------------------------------------===//
// URLs
//===--------------------------------------------------------------------===//

TEST(url_parse_basic) {
	WebHdfsUrl url;
	std::string error;
	CHECK(ParseWebHdfsUrl("webhdfs://NN1.example:9870/data//x/./y/", url, error));
	CHECK_EQ(url.scheme, "webhdfs");
	CHECK_EQ(url.authority, "nn1.example:9870");
	CHECK_EQ(url.host, "nn1.example");
	CHECK_EQ(url.port, 9870);
	CHECK_EQ(url.path, "/data/x/y");
	CHECK(!url.Secure());
	CHECK_EQ(url.ToString(), "webhdfs://nn1.example:9870/data/x/y");
}

TEST(url_parse_variants) {
	WebHdfsUrl url;
	std::string error;
	CHECK(ParseWebHdfsUrl("SWEBHDFS://cluster", url, error));
	CHECK(url.Secure());
	CHECK_EQ(url.path, "/");
	CHECK_EQ(url.port, 0);
	CHECK(ParseWebHdfsUrl("webhdfs://[::1]:9870/a", url, error));
	CHECK_EQ(url.host, "[::1]");
	CHECK_EQ(url.port, 9870);
	CHECK(!ParseWebHdfsUrl("webhdfs:///a", url, error));
	CHECK(!ParseWebHdfsUrl("hdfs://nn/a", url, error));
	CHECK(!ParseWebHdfsUrl("webhdfs://nn:99999/a", url, error));
	CHECK(!ParseWebHdfsUrl("webhdfs://nn:/a", url, error));
	CHECK(!ParseWebHdfsUrl("webhdfs://user@nn/a", url, error));
	// '?' is a glob wildcard, and '#' a plain character: every path byte is percent-encoded in requests
	CHECK(ParseWebHdfsUrl("webhdfs://nn/a?b#c", url, error));
	CHECK_EQ(url.path, "/a?b#c");
	CHECK(HasWebHdfsScheme("WebHdfs://x"));
	CHECK(!HasWebHdfsScheme("http://x"));
}

TEST(url_paths) {
	CHECK_EQ(ParentPath("/a/b"), "/a");
	CHECK_EQ(ParentPath("/a"), "/");
	CHECK_EQ(ParentPath("/"), "/");
	CHECK_EQ(BaseName("/a/b"), "b");
	CHECK_EQ(BaseName("/"), "");
	CHECK_EQ(JoinPath("/", "x"), "/x");
	CHECK_EQ(JoinPath("/a", "x"), "/a/x");
	CHECK_EQ(NormalizePath("//a///b/./"), "/a/b");
	CHECK_EQ(NormalizePath("/a/../.."), "/");
	std::string out;
	CHECK(TryNormalizePath("/a/b/../c/./..", out));
	CHECK_EQ(out, "/a");
	CHECK(TryNormalizePath("/a/..", out));
	CHECK_EQ(out, "/");
	CHECK(!TryNormalizePath("/..", out));
	CHECK(!TryNormalizePath("/a/../../b", out));
	// '...' and '..x' are ordinary names
	CHECK(TryNormalizePath("/.../..x", out));
	CHECK_EQ(out, "/.../..x");
}

TEST(url_parse_dot_segments) {
	// '..' is resolved while parsing, so allowed_directories and the request see the same path
	WebHdfsUrl url;
	std::string error;
	CHECK(ParseWebHdfsUrl("webhdfs://nn/ok/../secret/x.parquet", url, error));
	CHECK_EQ(url.path, "/secret/x.parquet");
	CHECK(ParseWebHdfsUrl("webhdfs://nn/ok/./sub/..", url, error));
	CHECK_EQ(url.path, "/ok");
	CHECK(!ParseWebHdfsUrl("webhdfs://nn/../etc", url, error));
	CHECK(error.find("goes above the root") != std::string::npos);
	CHECK(!ParseWebHdfsUrl("webhdfs://nn/a/../../etc", url, error));
}

TEST(url_scheme_roots_and_scopes) {
	// autocomplete lists "webhdfs:/" to complete what follows "webhdfs://"
	CHECK_EQ(SchemeRootOf("webhdfs:/"), "webhdfs");
	CHECK_EQ(SchemeRootOf("SWEBHDFS:/"), "swebhdfs");
	CHECK_EQ(SchemeRootOf("webhdfs:"), "");
	CHECK_EQ(SchemeRootOf("webhdfs://"), "");
	CHECK_EQ(SchemeRootOf("webhdfs:/x"), "");
	CHECK_EQ(SchemeRootOf("/tmp"), "");
	CHECK_EQ(ScopeAuthority("webhdfs://Prod", "webhdfs"), "prod");
	CHECK_EQ(ScopeAuthority("webhdfs://prod:9870/logs", "webhdfs"), "prod:9870");
	CHECK_EQ(ScopeAuthority("webhdfs://prod?x", "webhdfs"), "prod");
	CHECK_EQ(ScopeAuthority("webhdfs://", "webhdfs"), "");
	CHECK_EQ(ScopeAuthority("swebhdfs://prod", "webhdfs"), "");
	CHECK_EQ(ScopeAuthority("s3://bucket", "webhdfs"), "");
}

TEST(url_encoding) {
	CHECK_EQ(EncodePath("/a b/ü%#?&+="), "/a%20b/%C3%BC%25%23%3F%26%2B%3D");
	CHECK_EQ(EncodeQueryValue("a/b c&d"), "a%2Fb%20c%26d");
	CHECK_EQ(PercentDecode("a%20b%zz%4"), "a b%zz%4");
	CHECK_EQ(BuildRestUrl("http://nn:9870", "", "/x y", "OPEN", {{"offset", "10"}, {"user.name", "a b"}}),
	         "http://nn:9870/webhdfs/v1/x%20y?op=OPEN&offset=10&user.name=a%20b");
	CHECK_EQ(BuildRestUrl("https://knox:8443/", "/gateway/default/", "/", "LISTSTATUS", {}),
	         "https://knox:8443/gateway/default/webhdfs/v1/?op=LISTSTATUS");
}

TEST(url_query_rewrite) {
	auto url = "http://dn:9864/webhdfs/v1/f?op=OPEN&namenoderpcaddress=nn:8020&delegation=SECRET&offset=0";
	CHECK_EQ(ReplaceQueryParams(url, {{"offset", "100"}, {"length", "5"}}),
	         "http://dn:9864/webhdfs/v1/f?op=OPEN&namenoderpcaddress=nn:8020&delegation=SECRET&offset=100&length=5");
	std::string host, rest;
	CHECK(SplitUrlHost("http://dn:9864/webhdfs/v1/f?op=OPEN", host, rest));
	CHECK_EQ(host, "http://dn:9864");
	CHECK_EQ(rest, "/webhdfs/v1/f?op=OPEN");
}

//===--------------------------------------------------------------------===//
// Glob
//===--------------------------------------------------------------------===//

TEST(glob_component_matching) {
	CHECK(MatchComponent("file.parquet", "*.parquet"));
	CHECK(!MatchComponent("file.csv", "*.parquet"));
	CHECK(MatchComponent("abc", "a?c"));
	CHECK(MatchComponent("aüc", "a?c"));
	CHECK(MatchComponent("b", "[abc]"));
	CHECK(!MatchComponent("d", "[abc]"));
	CHECK(MatchComponent("d", "[!abc]"));
	CHECK(MatchComponent("d", "[^abc]"));
	CHECK(MatchComponent("5", "[0-9]"));
	CHECK(MatchComponent("a*b", "a\\*b"));
	CHECK(!MatchComponent("axb", "a\\*b"));
	CHECK(MatchComponent("[x", "[x"));
	CHECK(MatchComponent("", "*"));
	CHECK(MatchComponent("aaa", "*a*a*a*"));
	CHECK(!MatchComponent("aa", "*a*a*a*"));
	CHECK(MatchComponent("year=2024", "year=*"));
}

TEST(glob_braces) {
	auto expanded = ExpandBraces("/a/{b,c{d,e}}/f");
	CHECK_EQ(expanded.size(), 3u);
	CHECK_EQ(expanded[0], "/a/b/f");
	CHECK_EQ(expanded[1], "/a/cd/f");
	CHECK_EQ(expanded[2], "/a/ce/f");
	CHECK_EQ(ExpandBraces("/a/\\{b,c\\}").size(), 1u);
	CHECK_THROWS(ExpandBraces("/a/{b,c"));
	CHECK(HasGlobCharacters("/a/{b,c}"));
	CHECK(!HasGlobCharacters("/a/\\*"));
}

// A fake directory tree: path -> is_dir. Walks a GlobPattern over it like the real walker does.
struct FakeTree {
	std::map<std::string, bool> entries;

	void Add(const std::string &path, bool is_dir) {
		entries[path] = is_dir;
		auto parent = ParentPath(path);
		if (parent != path && !entries.count(parent)) {
			Add(parent, true);
		}
	}
	std::vector<std::pair<std::string, bool>> Children(const std::string &dir) const {
		std::vector<std::pair<std::string, bool>> result;
		for (auto &e : entries) {
			if (e.first != "/" && ParentPath(e.first) == dir) {
				result.emplace_back(BaseName(e.first), e.second);
			}
		}
		return result;
	}
	std::set<std::string> Glob(const std::string &pattern, bool include_hidden, bool files_only = true) const {
		auto plan = GlobPattern::Compile(pattern, include_hidden);
		std::set<std::string> result;
		if (plan.IsLiteral() || plan.MatchesRoot()) {
			if (entries.count(plan.Root()) && (!files_only || !entries.at(plan.Root()))) {
				result.insert(plan.Root());
			}
			if (plan.IsLiteral()) {
				return result;
			}
		}
		Walk(plan, plan.Root(), plan.Start(), result, files_only);
		return result;
	}
	void Walk(const GlobPattern &plan, const std::string &dir, const std::vector<GlobState> &states,
	          std::set<std::string> &result, bool files_only) const {
		for (auto &child : Children(dir)) {
			auto match = plan.Match(states, child.first, child.second);
			auto path = JoinPath(dir, child.first);
			if (match.emit && (!files_only || !child.second)) {
				result.insert(path);
			}
			if (child.second && !match.next.empty()) {
				Walk(plan, path, match.next, result, files_only);
			}
		}
	}
};

static FakeTree SampleTree() {
	FakeTree tree;
	tree.Add("/data/a.csv", false);
	tree.Add("/data/b.parquet", false);
	tree.Add("/data/_SUCCESS", false);
	tree.Add("/data/.hidden.parquet", false);
	tree.Add("/data/sub/c.parquet", false);
	tree.Add("/data/sub/deep/d.parquet", false);
	tree.Add("/data/_temporary/0/e.parquet", false);
	tree.Add("/data/year=2023/month=1/f.parquet", false);
	tree.Add("/data/year=2024/month=1/g.parquet", false);
	tree.Add("/data/year=2024/month=2/h.parquet", false);
	return tree;
}

TEST(glob_walk_basic) {
	auto tree = SampleTree();
	auto r = tree.Glob("/data/*.parquet", false);
	CHECK_EQ(r.size(), 1u);
	CHECK(r.count("/data/b.parquet"));
	r = tree.Glob("/data/**/*.parquet", false);
	CHECK_EQ(r.size(), 6u);
	CHECK(r.count("/data/b.parquet"));
	CHECK(r.count("/data/sub/deep/d.parquet"));
	CHECK(!r.count("/data/_temporary/0/e.parquet"));
	r = tree.Glob("/data/**", false);
	CHECK_EQ(r.size(), 7u);
	r = tree.Glob("/data/{a.csv,sub/c.parquet}", false);
	CHECK_EQ(r.size(), 2u);
	r = tree.Glob("/data/year=2024/*/*", false);
	CHECK_EQ(r.size(), 2u);
	r = tree.Glob("/data/year=202[3]/**", false);
	CHECK_EQ(r.size(), 1u);
}

TEST(glob_hidden_rules) {
	auto tree = SampleTree();
	auto r = tree.Glob("/data/*", false);
	CHECK(!r.count("/data/_SUCCESS"));
	CHECK(!r.count("/data/.hidden.parquet"));
	r = tree.Glob("/data/_*", false);
	CHECK(r.count("/data/_SUCCESS"));
	r = tree.Glob("/data/_SUCCESS", false);
	CHECK(r.count("/data/_SUCCESS"));
	r = tree.Glob("/data/*", true);
	CHECK(r.count("/data/_SUCCESS"));
	CHECK(r.count("/data/.hidden.parquet"));
	r = tree.Glob("/data/**/*.parquet", true);
	CHECK(r.count("/data/_temporary/0/e.parquet"));
	r = tree.Glob("/data/_temporary/**", false);
	CHECK_EQ(r.size(), 1u);
}

TEST(glob_roots) {
	auto plan = GlobPattern::Compile("/data/sub/*.parquet", false);
	CHECK_EQ(plan.Root(), "/data/sub");
	CHECK(!plan.IsLiteral());
	plan = GlobPattern::Compile("/data/{x,y}/z/*", false);
	CHECK_EQ(plan.Root(), "/data");
	plan = GlobPattern::Compile("/data/file", false);
	CHECK(plan.IsLiteral());
	CHECK_EQ(plan.Root(), "/data/file");
	plan = GlobPattern::Compile("/*", false);
	CHECK_EQ(plan.Root(), "/");
	CHECK_THROWS(GlobPattern::Compile("relative/*", false));
	// a '.' or '..' made by brace expansion or escaping would name a directory outside the pattern's
	CHECK_THROWS(GlobPattern::Compile("/ok/{..,x}/*", false));
	CHECK_THROWS(GlobPattern::Compile("/ok/{a/..,b}/*", false));
	CHECK_THROWS(GlobPattern::Compile("/ok/\\.\\./*", false));
	CHECK_THROWS(GlobPattern::Compile("/ok/{.,x}/\\./*", false));
	plan = GlobPattern::Compile("/ok/..?/*", false);
	CHECK_EQ(plan.Root(), "/ok");
}

TEST(glob_literal_children) {
	auto plan = GlobPattern::Compile("/t/year=2024/month={5,6}/*.parquet", false);
	CHECK_EQ(plan.Root(), "/t/year=2024");
	std::vector<std::string> names;
	CHECK(plan.LiteralChildren(plan.Start(), names));
	CHECK_EQ(names.size(), 2u);
	CHECK_EQ(names[0], "month=5");
	CHECK_EQ(names[1], "month=6");
	auto m = plan.Match(plan.Start(), "month=5", true);
	CHECK(!plan.LiteralChildren(m.next, names));
	plan = GlobPattern::Compile("/t/{a,b}/c/*", false);
	CHECK(plan.LiteralChildren(plan.Start(), names));
	m = plan.Match(plan.Start(), "a", true);
	CHECK(plan.LiteralChildren(m.next, names));
	CHECK_EQ(names[0], "c");
	plan = GlobPattern::Compile("/t/{a,b}", false);
	CHECK(!plan.LiteralChildren(plan.Start(), names));
	plan = GlobPattern::Compile("/t/*/x", false);
	CHECK(!plan.LiteralChildren(plan.Start(), names));
	plan = GlobPattern::Compile("/t/{a,*}/x", false);
	CHECK(!plan.LiteralChildren(plan.Start(), names));
}

//===--------------------------------------------------------------------===//
// Hadoop config
//===--------------------------------------------------------------------===//

TEST(hadoop_config_ha) {
	HadoopConfig config;
	std::string error;
	CHECK(config.ParseXml(R"(<?xml version="1.0"?>
<configuration>
  <!-- <property><name>ignored</name><value>x</value></property> -->
  <property><name>dfs.nameservices</name><value>mycluster</value></property>
  <property>
    <name>dfs.ha.namenodes.mycluster</name>
    <value> nn1 , nn2 </value>
  </property>
  <property><name>dfs.namenode.http-address.mycluster.nn1</name><value>host1:9870</value></property>
  <property><name>dfs.namenode.http-address.mycluster.nn2</name><value>host2:9870</value></property>
  <property><name>dfs.namenode.https-address.mycluster.nn2</name><value>host2:9871</value></property>
  <property><name>dfs.namenode.http-address.single</name><value>solo:9870</value></property>
  <property><name>escaped</name><value>a&amp;b</value></property>
</configuration>)",
	                      error));
	CHECK(!config.Has("ignored"));
	CHECK_EQ(config.Get("escaped"), "a&b");
	auto nns = config.NameNodeAddresses("mycluster", false);
	CHECK_EQ(nns.size(), 2u);
	CHECK_EQ(nns[0], "host1:9870");
	CHECK_EQ(nns[1], "host2:9870");
	nns = config.NameNodeAddresses("mycluster", true);
	CHECK_EQ(nns.size(), 1u);
	CHECK_EQ(config.NameNodeAddresses("single", false)[0], "solo:9870");
	CHECK(config.NameNodeAddresses("unknown", false).empty());
	CHECK(!config.ParseXml("<property><name>x</name>", error));
}

//===--------------------------------------------------------------------===//
// JSON
//===--------------------------------------------------------------------===//

TEST(json_file_status) {
	FileStatus status;
	std::string error;
	CHECK(ParseFileStatus(R"({"FileStatus":{"accessTime":1700000000000,"blockSize":134217728,"childrenNum":0,
		"fileId":16390,"group":"supergroup","length":1234,"modificationTime":1700000001000,"owner":"hadoop",
		"pathSuffix":"","permission":"644","replication":3,"storagePolicy":0,"type":"FILE"}})",
	                      status, error));
	CHECK(!status.is_dir);
	CHECK_EQ(status.length, 1234);
	CHECK_EQ(status.modification_time, 1700000001000LL);
	CHECK_EQ(status.permission, 0644);
	CHECK_EQ(status.replication, 3);
	CHECK_EQ(FormatPermission(status.permission), "rw-r--r--");
	CHECK_EQ(FormatPermission(01777), "rwxrwxrwt");
	CHECK_EQ(FormatPermission(01776), "rwxrwxrwT");
	CHECK(!ParseFileStatus("not json", status, error));
	CHECK(!ParseFileStatus("{}", status, error));
}

TEST(json_listing) {
	std::vector<FileStatus> list;
	int64_t remaining = -1;
	std::string error;
	CHECK(ParseFileStatusList(R"({"FileStatuses":{"FileStatus":[
		{"pathSuffix":"a","type":"DIRECTORY","length":0,"permission":"755","owner":"o","group":"g",
		 "modificationTime":1,"accessTime":0,"blockSize":0,"replication":0},
		{"pathSuffix":"b","type":"FILE","length":5,"permission":"600","owner":"o","group":"g",
		 "modificationTime":2,"accessTime":3,"blockSize":10,"replication":1}]}})",
	                          list, remaining, error));
	CHECK_EQ(list.size(), 2u);
	CHECK(list[0].is_dir);
	CHECK_EQ(list[0].replication, -1);
	CHECK_EQ(list[1].path_suffix, "b");
	CHECK_EQ(remaining, 0);
	list.clear();
	CHECK(ParseFileStatusList(R"({"DirectoryListing":{"partialListing":{"FileStatuses":{"FileStatus":[
		{"pathSuffix":"c","type":"FILE","length":1,"permission":"644"}]}},"remainingEntries":7}})",
	                          list, remaining, error));
	CHECK_EQ(list.size(), 1u);
	CHECK_EQ(remaining, 7);
	list.clear();
	CHECK(ParseFileStatusList(R"({"FileStatuses":{"FileStatus":[]}})", list, remaining, error));
	CHECK(list.empty());
	// a listed name must be a plain name: it is joined to the listed directory
	for (auto name : {".", "..", "a/b", "../x"}) {
		std::string body =
		    std::string(R"({"FileStatuses":{"FileStatus":[{"pathSuffix":")") + name + R"(","type":"DIRECTORY"}]}})";
		error.clear();
		CHECK(!ParseFileStatusList(body, list, remaining, error));
		CHECK(error.find("invalid pathSuffix") != std::string::npos);
	}
	CHECK(!ParseFileStatusList(R"({"FileStatuses":{"FileStatus":[{"pathSuffix":"a\u0000b","type":"FILE"}]}})", list,
	                           remaining, error));
	CHECK(IsValidPathSuffix(""));
	CHECK(IsValidPathSuffix("..."));
	CHECK(IsValidPathSuffix(".hidden"));
}

TEST(json_file_status_times) {
	FileStatus status;
	std::string error;
	// HDFS reports accessTime 0 when it doesn't track it; a missing field reads the same
	CHECK(ParseFileStatus(R"({"FileStatus":{"pathSuffix":"","type":"DIRECTORY","accessTime":0,
		"modificationTime":5}})",
	                      status, error));
	CHECK_EQ(status.access_time, 0);
	CHECK_EQ(status.modification_time, 5);
	CHECK(ParseFileStatus(R"({"FileStatus":{"pathSuffix":"","type":"FILE","length":1}})", status, error));
	CHECK_EQ(status.access_time, 0);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","modificationTime":9223372036854775807}})", status, error));
	CHECK(error.find("modificationTime") != std::string::npos);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","accessTime":-9223372036854775807}})", status, error));
	// the bounds are DuckDB's TIMESTAMP range, which is not symmetric
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","modificationTime":-9223372022400000}})", status, error));
	CHECK_EQ(status.modification_time, MIN_EPOCH_MS);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","modificationTime":-9223372022400001}})", status, error));
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","modificationTime":-9223372036854774}})", status, error));
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","modificationTime":9223372036854775}})", status, error));
	CHECK_EQ(status.modification_time, MAX_EPOCH_MS);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","modificationTime":9223372036854776}})", status, error));
}

TEST(version_tag) {
	FileStatus a, b;
	std::string error, tag_a, tag_b;
	// two files of the same length and mtime are told apart by their fileId
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":3,"modificationTime":5,"fileId":7}})", a, error));
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":3,"modificationTime":5,"fileId":8}})", b, error));
	CHECK(ListingEtag(a, tag_a));
	CHECK(ListingEtag(b, tag_b));
	CHECK_EQ(tag_a, std::string("7:5:3"));
	CHECK(tag_a != tag_b);
	// the listing's etag is the handle's version tag
	CHECK_EQ(tag_a, VersionTag(7, 5, 3));
	// no fileId from the server: -1
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":3,"modificationTime":5}})", a, error));
	CHECK(ListingEtag(a, tag_a));
	CHECK_EQ(tag_a, std::string("-1:5:3"));
	// a negative fileId is no id either
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":3,"modificationTime":5,"fileId":-5}})", a, error));
	CHECK(ListingEtag(a, tag_a));
	CHECK_EQ(tag_a, std::string("-1:5:3"));
	// a negative length has no etag
	a.length = -1;
	CHECK(!ListingEtag(a, tag_a));
}

TEST(xattr_hex_values) {
	std::string bytes;
	CHECK(DecodeHex("0x626c7565", bytes));
	CHECK_EQ(bytes, "blue");
	CHECK(DecodeHex("0X00FF", bytes));
	CHECK_EQ(bytes, std::string("\x00\xff", 2));
	CHECK(DecodeHex("", bytes));
	CHECK_EQ(bytes, "");
	// strtol would accept a sign or leading whitespace in a pair
	CHECK(!DecodeHex("0x-1", bytes));
	CHECK(!DecodeHex("0x+f", bytes));
	CHECK(!DecodeHex("0x f", bytes));
	CHECK(!DecodeHex("0xabc", bytes));
	CHECK(!DecodeHex("0xzz", bytes));
}

TEST(walk_order) {
	// directories sort as "name/", so a depth-first walk lists full paths in byte order
	std::vector<std::pair<std::string, bool>> children = {{"a", true},  {"a.csv", false}, {"a-b", true},
	                                                      {"b", false}, {"ab", true},     {"\xc3\xbc", false}};
	std::sort(children.begin(), children.end(),
	          [](const std::pair<std::string, bool> &x, const std::pair<std::string, bool> &y) {
		          return WalkOrderLess(x.first, x.second, y.first, y.second);
	          });
	std::vector<std::string> order;
	for (auto &child : children) {
		order.push_back(child.first + (child.second ? "/" : ""));
	}
	auto sorted = order;
	std::sort(sorted.begin(), sorted.end());
	CHECK(order == sorted);
	CHECK_EQ(order[0], "a-b/");
	CHECK_EQ(order[1], "a.csv");
	CHECK_EQ(order[2], "a/");
	CHECK(!WalkOrderLess("x", true, "x", true));
	CHECK(WalkOrderLess("x", false, "x-", false));
	CHECK(WalkOrderLess("x-", false, "x", true));
}

TEST(json_checked_integers) {
	FileStatus status;
	std::string error;
	// hostile numbers are errors, not wrapped values or undefined behaviour
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":18446744073709551615}})", status, error));
	CHECK(error.find("length") != std::string::npos);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":1e300}})", status, error));
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":"99999999999999999999"}})", status, error));
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":-1}})", status, error));
	CHECK(error.find("negative length") != std::string::npos);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","replication":4294967296}})", status, error));
	CHECK(error.find("replication") != std::string::npos);
	CHECK(!ParseFileStatus(R"({"FileStatus":{"type":"FILE","storagePolicy":-2147483649}})", status, error));
	CHECK(ParseFileStatus(R"({"FileStatus":{"type":"FILE","length":1.0e3,"replication":"3"}})", status, error));
	CHECK_EQ(status.length, 1000);
	CHECK_EQ(status.replication, 3);
	int64_t remaining = 0;
	std::vector<FileStatus> list;
	CHECK(!ParseFileStatusList(R"({"DirectoryListing":{"partialListing":{"FileStatuses":{"FileStatus":[]}},
		"remainingEntries":1e300}})",
	                           list, remaining, error));
	int64_t l = 0;
	CHECK(!ParseLongResult(R"({"long":18446744073709551615})", l, error));
	auto doc = JsonDocument::Parse(R"([9223372036854775807, 9223372036854775808, -1e300, "12", "x", 1.5])", error);
	CHECK(doc != nullptr);
	auto arr = doc->Root();
	int64_t v = 0;
	CHECK(arr.At(0).TryGetInt(v));
	CHECK_EQ(v, INT64_MAX);
	CHECK(!arr.At(1).TryGetInt(v));
	CHECK_EQ(arr.At(1).GetInt(-7), -7);
	CHECK(!arr.At(2).TryGetInt(v));
	CHECK(arr.At(3).TryGetInt(v));
	CHECK_EQ(v, 12);
	CHECK(arr.At(4).TryGetInt(v, 4));
	CHECK_EQ(v, 4);
	CHECK(arr.At(5).TryGetInt(v));
	CHECK_EQ(v, 1);
	CHECK_EQ(arr.At(1).GetDouble(), 9223372036854775808.0);
}

TEST(json_misc) {
	RemoteException ex;
	CHECK(ParseRemoteException(R"({"RemoteException":{"exception":"FileNotFoundException",
		"javaClassName":"java.io.FileNotFoundException","message":"File does not exist: /x"}})",
	                           ex));
	CHECK_EQ(ex.exception, "FileNotFoundException");
	CHECK_EQ(ex.message, "File does not exist: /x");
	CHECK(!ParseRemoteException("<html>", ex));
	bool b = false;
	std::string error;
	CHECK(ParseBooleanResult(R"({"boolean":true})", b, error));
	CHECK(b);
	std::string location;
	CHECK(ParseLocation(R"({"Location":"http://dn:9864/x"})", location));
	CHECK_EQ(location, "http://dn:9864/x");
	std::string token;
	CHECK(ParseToken(R"({"Token":{"urlString":"abc"}})", token, error));
	CHECK_EQ(token, "abc");
	int64_t l = 0;
	CHECK(ParseLongResult(R"({"long":42})", l, error));
	CHECK_EQ(l, 42);
	auto doc = JsonDocument::Parse(R"({"a":{"b":[1,"x",true,null]}})", error);
	CHECK(doc != nullptr);
	auto arr = doc->Root()["a"]["b"];
	CHECK_EQ(arr.Size(), 4u);
	CHECK_EQ(arr.At(0).GetInt(), 1);
	CHECK_EQ(arr.At(1).GetString(), "x");
	CHECK(arr.At(2).GetBool());
	CHECK(arr.At(3).IsNull());
	CHECK(arr.At(9).IsMissing());
	CHECK_EQ(doc->Root()["a"].ToJson(), R"({"b":[1,"x",true,null]})");
}

//===--------------------------------------------------------------------===//
// LISTSTATUS_BATCH paging
//===--------------------------------------------------------------------===//

//! Feed canned pages through the same checks as WebHdfsClient::ListStatus; returns false at the first page that
//! does not advance
static bool PageThrough(const std::vector<std::string> &pages, std::string &error) {
	std::string start_after;
	bool first = true;
	std::unordered_set<std::string> seen;
	for (auto &body : pages) {
		std::vector<FileStatus> list;
		int64_t remaining = 0;
		CHECK(ParseFileStatusList(body, list, remaining, error));
		auto last = list.empty() ? std::string() : list.back().path_suffix;
		if (!ListingPageAdvances(first, seen, list.size(), last, remaining, error)) {
			return false;
		}
		first = false;
		if (!list.empty()) {
			start_after = last;
		}
		if (remaining <= 0 || list.empty() || start_after.empty()) {
			return true;
		}
	}
	return true;
}

static std::string BatchPage(const std::vector<std::string> &names, int64_t remaining) {
	std::string body = R"({"DirectoryListing":{"partialListing":{"FileStatuses":{"FileStatus":[)";
	for (size_t i = 0; i < names.size(); i++) {
		body += (i ? "," : "") + std::string(R"({"pathSuffix":")") + names[i] + R"(","type":"FILE","length":1})";
	}
	return body + R"(]}},"remainingEntries":)" + std::to_string(remaining) + "}}";
}

TEST(liststatus_batch_paging) {
	std::string error;
	// a normal two-page listing
	CHECK(PageThrough({BatchPage({"a", "b"}, 2), BatchPage({"c", "d"}, 0)}, error));
	// an empty directory, and a listed file (named "")
	CHECK(PageThrough({BatchPage({}, 0)}, error));
	CHECK(PageThrough({BatchPage({""}, 0)}, error));
	// the second page repeats the first page's last name: would request the same page forever
	CHECK(!PageThrough({BatchPage({"a", "b"}, 2), BatchPage({"b"}, 1), BatchPage({"b"}, 1)}, error));
	CHECK(error.find("the page ending at \"b\" repeats an earlier page") != std::string::npos);
	// a server that alternates between two pages: each repeats the page before the previous one
	CHECK(!PageThrough({BatchPage({"a"}, 1), BatchPage({"b"}, 1), BatchPage({"a"}, 1), BatchPage({"b"}, 1)}, error));
	CHECK(error.find("the page ending at \"a\" repeats an earlier page") != std::string::npos);
	// an empty page while entries remain
	CHECK(!PageThrough({BatchPage({"a"}, 3), BatchPage({}, 3)}, error));
	CHECK(error.find("empty page with 3 entries remaining") != std::string::npos);
	// a nameless last entry while entries remain
	CHECK(!PageThrough({BatchPage({""}, 5)}, error));
	// no ordering is assumed: the NameNode compares names as signed bytes, so non-ASCII UTF-8 names come before
	// ASCII ones ("\xc3\xbc" < "a"), and either order across a page boundary is valid
	CHECK(PageThrough({BatchPage({"y", "z"}, 1), BatchPage({"\xc3\xbc"}, 0)}, error));
	CHECK(PageThrough({BatchPage({"\xc3\xbc"}, 1), BatchPage({"a"}, 0)}, error));
}

int main() {
	return unit::RunAll();
}

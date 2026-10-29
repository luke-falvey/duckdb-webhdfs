// Cluster-free unit tests: URL handling, glob matching, Hadoop config parsing and WebHDFS JSON parsing.

#include "test_main.hpp"
#include "webhdfs_glob.hpp"
#include "webhdfs_hadoop_config.hpp"
#include "webhdfs_json.hpp"
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

TEST(url_query_rewrite_and_redact) {
	auto url = "http://dn:9864/webhdfs/v1/f?op=OPEN&namenoderpcaddress=nn:8020&delegation=SECRET&offset=0";
	CHECK_EQ(ReplaceQueryParams(url, {{"offset", "100"}, {"length", "5"}}),
	         "http://dn:9864/webhdfs/v1/f?op=OPEN&namenoderpcaddress=nn:8020&delegation=SECRET&offset=100&length=5");
	CHECK_EQ(RedactUrl(url), "http://dn:9864/webhdfs/v1/f?op=OPEN&namenoderpcaddress=nn:8020&delegation=REDACTED&"
	                         "offset=0");
	CHECK_EQ(RedactUrl("http://nn/x?op=X&user.name=bob&doas=al"),
	         "http://nn/x?op=X&user.name=REDACTED&doas=REDACTED");
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

int main() {
	return unit::RunAll();
}

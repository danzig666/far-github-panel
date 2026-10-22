// Unit tests for the platform-neutral core. No network, no Far.

#include <algorithm>
#include <memory>

#include "../src/core/GitHubClient.hpp"
#include "../src/core/Json.hpp"
#include "../src/core/Location.hpp"
#include "../src/core/RepoTree.hpp"
#include "../src/core/Text.hpp"
#include "../src/core/Url.hpp"
#include "TestMain.hpp"

using namespace gh;

namespace {

// Replies are matched by URL prefix; unmatched requests get 404.
struct FakeTransport : HttpTransport {
  struct Rule {
    std::string urlPrefix;
    HttpResponse reply;
  };
  std::vector<Rule> rules;
  std::vector<HttpRequest> requests;

  void On(const std::string& prefix, int status, std::string body, std::vector<HttpHeader> headers = {}) {
    rules.push_back({prefix, HttpResponse{status, std::move(headers), std::move(body)}});
  }

  HttpResponse Send(HttpRequest& request) override {
    requests.push_back(request);
    for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
      if (request.url.rfind(it->urlPrefix, 0) != 0) continue;
      HttpResponse reply = it->reply;
      if (reply.status >= 200 && reply.status < 300) {
        if (request.onHeaders && !request.onHeaders(reply)) throw CancelledError();
        if (request.sink) {
          if (!request.sink(reply.body.data(), reply.body.size())) throw CancelledError();
          reply.body.clear();
        }
      }
      return reply;
    }
    return HttpResponse{404, {}, R"({"message":"Not Found"})"};
  }

  std::string header(size_t i, const std::string& name) const {
    for (const auto& h : requests[i].headers)
      if (h.name == name) return h.value;
    return {};
  }
};

std::string Collect(const std::function<void(const DownloadSink&)>& fn, uint64_t* size = nullptr) {
  std::string out;
  DownloadSink sink;
  sink.onSize = [&](uint64_t total) {
    if (size) *size = total;
  };
  sink.onData = [&](const char* d, size_t n) {
    out.append(d, n);
    return true;
  };
  fn(sink);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Text and URLs

TEST("iso8601 parsing") {
  CHECK_EQ(ParseIso8601("1970-01-01T00:00:00Z"), 0);
  CHECK_EQ(ParseIso8601("2026-10-22T17:23:17Z"), 1792689797LL);
  CHECK_EQ(ParseIso8601("2026-10-22T19:23:17+02:00"), 1792689797LL);
  CHECK_EQ(ParseIso8601("2026-10-22T17:23:17.123Z"), 1792689797LL);
  CHECK_EQ(ParseIso8601("garbage"), 0);
}

TEST("url parsing and redirects") {
  Url u;
  CHECK(ParseUrl("https://api.github.com/repos/a/b?x=1", u));
  CHECK(u.https);
  CHECK_EQ(u.host, std::string("api.github.com"));
  CHECK_EQ(u.port, 443);
  CHECK_EQ(u.target, std::string("/repos/a/b?x=1"));
  CHECK(ParseUrl("http://localhost:8080", u));
  CHECK_EQ(u.port, 8080);
  CHECK_EQ(u.target, std::string("/"));
  CHECK(!ParseUrl("ftp://x", u));
  CHECK_EQ(ResolveLocation("https://api.github.com/a/b", "https://codeload.github.com/z"),
           std::string("https://codeload.github.com/z"));
  CHECK_EQ(ResolveLocation("https://api.github.com/a/b", "/c"), std::string("https://api.github.com/c"));
}

TEST("link header next") {
  std::string link =
      R"(<https://api.github.com/user/repos?page=2>; rel="next", <https://api.github.com/user/repos?page=9>; rel="last")";
  CHECK_EQ(NextLink(link), std::string("https://api.github.com/user/repos?page=2"));
  CHECK_EQ(NextLink(R"(<https://x?page=1>; rel="prev", <https://x?page=3>; rel="next")"), std::string("https://x?page=3"));
  CHECK_EQ(NextLink(R"(<https://x?page=1>; rel="first")"), std::string());
}

TEST("path encoding keeps slashes") {
  CHECK_EQ(EncodePath("feature/a b#1"), std::string("feature/a%20b%231"));
  CHECK_EQ(SplitPath("/a//b/"), (std::vector<std::string>{"a", "b"}));
}

// ---------------------------------------------------------------------------
// Locations

TEST("location classification") {
  CHECK(Classify({}).level == Level::Root);
  CHECK(Classify({"o"}).level == Level::Owner);
  Location t = Classify({"o", "r", "src", "core"});
  CHECK(t.level == Level::Tree);
  CHECK(t.refKind == RefKind::Default);
  CHECK_EQ(t.path, std::string("src/core"));
  CHECK(Classify({"o", "r", "[branches]"}).level == Level::Branches);
  Location b = Classify({"o", "r", "[branches]", "feature/x", "docs"});
  CHECK(b.level == Level::Tree);
  CHECK(b.refKind == RefKind::Branch);
  CHECK_EQ(b.ref, std::string("feature/x"));
  CHECK_EQ(b.path, std::string("docs"));
  CHECK(Classify({"o", "r", "[tags]", "v1"}).refKind == RefKind::Tag);
  CHECK(Classify({"o", "r", "[releases]"}).level == Level::Releases);
  Location rel = Classify({"o", "r", "[releases]", "v1.0"});
  CHECK(rel.level == Level::Release);
  CHECK_EQ(rel.release, std::string("v1.0"));
}

TEST("ref display names") {
  std::wstring shown = RefToDisplay(L"feature/x");
  CHECK(shown.find(L'/') == std::wstring::npos);
  CHECK(RefFromDisplay(shown) == std::wstring(L"feature/x"));
}

TEST("github.com urls") {
  GitHubUrl u;
  CHECK(ParseGitHubUrl("https://github.com/danzig666/far-ps5-filemanager/tree/feature/x/src/core", u));
  CHECK(u.kind == GitHubUrl::Kind::Tree);
  CHECK_EQ(u.owner, std::string("danzig666"));
  CHECK_EQ(u.repo, std::string("far-ps5-filemanager"));
  CHECK_EQ(u.rest, std::string("feature/x/src/core"));
  auto splits = RefSplits(u.rest);
  CHECK_EQ(splits.size(), 4u);
  CHECK_EQ(splits[0].first, std::string("feature/x/src/core"));
  CHECK_EQ(splits[2].first, std::string("feature/x"));
  CHECK_EQ(splits[2].second, std::string("src/core"));
  CHECK(ParseGitHubUrl("https://github.com/o/r.git", u));
  CHECK(u.kind == GitHubUrl::Kind::Repo);
  CHECK_EQ(u.repo, std::string("r"));
  CHECK(ParseGitHubUrl("https://github.com/o/r/releases/tag/v1.2", u));
  CHECK(u.kind == GitHubUrl::Kind::Release);
  CHECK_EQ(u.release, std::string("v1.2"));
  CHECK(!ParseGitHubUrl("https://gitlab.com/o/r", u));
}

// ---------------------------------------------------------------------------
// Client

TEST("client sends api headers and token") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/user", 200, R"({"login":"me"})",
        {{"X-RateLimit-Limit", "5000"}, {"X-RateLimit-Remaining", "4999"}, {"X-RateLimit-Reset", "123"},
         {"X-OAuth-Scopes", "repo, read:org"}});
  GitHubClient c(t, "tok", "test-agent");
  CHECK_EQ(c.CurrentLogin(), std::string("me"));
  CHECK_EQ(t->header(0, "Authorization"), std::string("Bearer tok"));
  CHECK_EQ(t->header(0, "Accept"), std::string("application/vnd.github+json"));
  CHECK_EQ(t->header(0, "X-GitHub-Api-Version"), std::string("2022-11-28"));
  CHECK_EQ(t->header(0, "User-Agent"), std::string("test-agent"));
  CHECK_EQ(c.Rate().remaining, 4999);
  CHECK_EQ(c.Rate().scopes, std::string("repo, read:org"));
  GitHubClient anon(t);
  anon.CurrentLogin();
  CHECK_EQ(t->header(1, "Authorization"), std::string());
}

TEST("client follows pagination up to the limit") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/users/o/repos", 200, R"([{"name":"a","owner":{"login":"o"}},{"name":"b"}])",
        {{"Link", R"(<https://api.github.com/users/o/repos?page=2>; rel="next")"}});
  t->On("https://api.github.com/users/o/repos?page=2", 200,
        R"([{"name":"c","private":true,"stargazers_count":7,"pushed_at":"1970-01-02T00:00:00Z"}])");
  GitHubClient c(t);
  bool more = true;
  auto repos = c.ListRepos("o", OwnerKind::User, 100, &more);
  CHECK_EQ(repos.size(), 3u);
  CHECK(!more);
  CHECK_EQ(repos[2].name, std::string("c"));
  CHECK(repos[2].isPrivate);
  CHECK_EQ(repos[2].stars, 7u);
  CHECK_EQ(repos[2].pushedAt, 86400);
  CHECK(t->requests[0].url.find("per_page=100") != std::string::npos);
  CHECK(t->requests[0].url.find("/users/o/repos?sort=pushed&type=owner") != std::string::npos);
  auto limited = c.ListRepos("o", OwnerKind::User, 2, &more);
  CHECK_EQ(limited.size(), 2u);
  CHECK(more);
}

TEST("client reuses bodies on 304") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/repos/o/r", 200, R"({"name":"r","default_branch":"main"})", {{"ETag", "W/\"abc\""}});
  GitHubClient c(t);
  CHECK_EQ(c.GetRepo("o", "r").defaultBranch, std::string("main"));
  t->On("https://api.github.com/repos/o/r", 304, "");
  CHECK_EQ(c.GetRepo("o", "r").defaultBranch, std::string("main"));
  CHECK_EQ(t->header(1, "If-None-Match"), std::string("W/\"abc\""));
  CHECK_EQ(c.notModifiedCount(), 1u);
}

TEST("client errors and rate limits") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/repos/o/missing", 404, R"({"message":"Not Found"})");
  t->On("https://api.github.com/repos/o/limited", 403, R"({"message":"API rate limit exceeded for 1.2.3.4."})",
        {{"X-RateLimit-Remaining", "0"}, {"X-RateLimit-Reset", "1700000000"}, {"X-RateLimit-Limit", "60"}});
  GitHubClient c(t);
  try {
    c.GetRepo("o", "missing");
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK_EQ(e.status(), 404);
    CHECK(!e.rateLimited());
  }
  try {
    c.GetRepo("o", "limited");
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK(e.rateLimited());
    CHECK_EQ(e.resetAt(), 1700000000);
    CHECK_EQ(c.Rate().remaining, 0);
  }
}

TEST("resolve ref uses the sha media type then the git commit") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/repos/o/r/commits/feature/x", 200, "0123abc\n");
  t->On("https://api.github.com/repos/o/r/git/commits/0123abc", 200,
        R"({"sha":"0123abc","tree":{"sha":"tree1"},"committer":{"date":"1970-01-01T01:00:00Z"}})");
  GitHubClient c(t);
  Commit commit = c.ResolveRef("o", "r", "feature/x");
  CHECK_EQ(commit.sha, std::string("0123abc"));
  CHECK_EQ(commit.treeSha, std::string("tree1"));
  CHECK_EQ(commit.date, 3600);
  CHECK_EQ(t->header(0, "Accept"), std::string("application/vnd.github.sha"));
}

TEST("downloads follow redirects and drop the token on another host") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/repos/o/r/releases/assets/7", 302, "",
        {{"Location", "https://objects.githubusercontent.com/signed?x=1"}});
  t->On("https://objects.githubusercontent.com/signed", 200, "ASSET", {{"Content-Length", "5"}});
  GitHubClient c(t, "secret");
  uint64_t size = 0;
  std::string body = Collect([&](const DownloadSink& s) { c.DownloadAsset("o", "r", 7, s); }, &size);
  CHECK_EQ(body, std::string("ASSET"));
  CHECK_EQ(size, 5u);
  CHECK_EQ(t->requests.size(), 2u);
  CHECK_EQ(t->header(0, "Authorization"), std::string("Bearer secret"));
  CHECK_EQ(t->header(0, "Accept"), std::string("application/octet-stream"));
  CHECK_EQ(t->header(1, "Authorization"), std::string());
}

TEST("blob download asks for raw content") {
  auto t = std::make_shared<FakeTransport>();
  t->On("https://api.github.com/repos/o/r/git/blobs/s1", 200, "hello");
  GitHubClient c(t, "tok");
  CHECK_EQ(Collect([&](const DownloadSink& s) { c.DownloadBlob("o", "r", "s1", s); }), std::string("hello"));
  CHECK_EQ(t->header(0, "Accept"), std::string("application/vnd.github.raw"));
}

TEST("lfs pointer detection") {
  uint64_t size = 0;
  std::string pointer =
      "version https://git-lfs.github.com/spec/v1\noid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\nsize 12345\n";
  CHECK(ParseLfsPointer(pointer, &size));
  CHECK_EQ(size, 12345u);
  CHECK(!ParseLfsPointer("version 1\n"));
  CHECK(!ParseLfsPointer("hello"));
}

// ---------------------------------------------------------------------------
// Tree model

namespace {
Tree MakeTree(std::vector<TreeEntry> entries, bool truncated = false) {
  Tree t;
  t.truncated = truncated;
  t.entries = std::move(entries);
  return t;
}
}  // namespace

TEST("tree from a complete recursive listing") {
  RepoTree tree("root", MakeTree({{"README.md", "100644", "blob", "b1", 10},
                                  {"src", "040000", "tree", "t1", 0},
                                  {"src/main.cpp", "100755", "blob", "b2", 20},
                                  {"src/empty", "040000", "tree", "t2", 0},
                                  {"lib", "160000", "commit", "c1", 0}}));
  int fetches = 0;
  RepoTree::Fetch fetch = [&](const std::string&) {
    ++fetches;
    return Tree{};
  };
  CHECK_EQ(tree.Children("", fetch).size(), 3u);
  CHECK_EQ(tree.Children("src", fetch).size(), 2u);
  CHECK(tree.Children("src/empty", fetch).empty());
  const TreeNode* main = tree.Find("src/main.cpp", fetch);
  CHECK(main && main->isExecutable() && main->size == 20);
  CHECK(tree.Find("lib", fetch)->isSubmodule());
  std::vector<std::string> walked;
  tree.Walk("", fetch, [&](const TreeNode& n) { walked.push_back(n.path); });
  CHECK_EQ(walked.size(), 5u);
  CHECK_EQ(walked[1], std::string("src"));
  CHECK_EQ(walked[2], std::string("src/main.cpp"));
  CHECK_EQ(fetches, 0);
  CHECK_THROWS(tree.Children("nope", fetch), ApiError);
}

TEST("truncated tree loads folders one at a time") {
  RepoTree tree("root", MakeTree({{"a", "040000", "tree", "ta", 0}}, true));
  std::vector<std::string> fetched;
  RepoTree::Fetch fetch = [&](const std::string& sha) {
    fetched.push_back(sha);
    if (sha == "root") return MakeTree({{"a", "040000", "tree", "ta", 0}, {"z.txt", "100644", "blob", "bz", 1}});
    if (sha == "ta") return MakeTree({{"deep.txt", "100644", "blob", "bd", 2}});
    return Tree{};
  };
  CHECK(tree.truncated());
  CHECK_EQ(tree.Children("", fetch).size(), 2u);
  const TreeNode* deep = tree.Find("a/deep.txt", fetch);
  CHECK(deep && deep->path == "a/deep.txt");
  CHECK_EQ(fetched.size(), 2u);
  tree.Children("a", fetch);
  CHECK_EQ(fetched.size(), 2u);  // cached
}

int main() {
  return test::RunAll();
}

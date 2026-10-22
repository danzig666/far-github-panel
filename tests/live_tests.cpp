// Live tests against api.github.com. Built twice: on Linux with the libcurl
// transport and on Windows with the plugin's WinHTTP transport. Uses
// GITHUB_TOKEN from the environment if set (CI), else runs anonymously.
// Only public repositories are touched.

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "../src/core/GitHubClient.hpp"
#include "../src/core/RepoTree.hpp"
#include "TestMain.hpp"

#ifdef _WIN32
#include "../src/far/WinHttpTransport.hpp"
#else
#include "CurlHttp.hpp"
#endif

using namespace gh;

namespace {

std::shared_ptr<HttpTransport> BaseTransport() {
#ifdef _WIN32
  static auto t = std::make_shared<WinHttpTransport>();
#else
  static auto t = std::make_shared<CurlHttpTransport>();
#endif
  return t;
}

// Records which hosts received an Authorization header.
struct Recorder : HttpTransport {
  struct Seen {
    std::string url;
    bool auth;
  };
  std::vector<Seen> seen;
  HttpResponse Send(HttpRequest& request) override {
    bool auth = std::any_of(request.headers.begin(), request.headers.end(),
                            [](const HttpHeader& h) { return h.name == "Authorization"; });
    seen.push_back({request.url, auth});
    return BaseTransport()->Send(request);
  }
};

std::string Token() {
  const char* t = std::getenv("GITHUB_TOKEN");
  return t ? t : "";
}

GitHubClient Client() {
  return GitHubClient(BaseTransport(), Token(), "far-github-panel-tests");
}

std::string Fetch(const std::function<void(const DownloadSink&)>& fn) {
  std::string out;
  DownloadSink sink;
  sink.onData = [&](const char* d, size_t n) {
    out.append(d, n);
    return true;
  };
  fn(sink);
  return out;
}

}  // namespace

TEST("owner types") {
  GitHubClient c = Client();
  CHECK(c.OwnerType("github") == OwnerKind::Organization);
  CHECK(c.OwnerType("octocat") == OwnerKind::User);
  CHECK(c.Rate().limit > 0);
}

TEST("list a user's repositories") {
  GitHubClient c = Client();
  auto repos = c.ListRepos("octocat", OwnerKind::User, 500);
  CHECK(std::any_of(repos.begin(), repos.end(), [](const Repo& r) { return r.name == "Hello-World"; }));
}

TEST("tree and blob of octocat/Hello-World") {
  GitHubClient c = Client();
  Repo repo = c.GetRepo("octocat", "Hello-World");
  CHECK_EQ(repo.defaultBranch, std::string("master"));
  Commit commit = c.ResolveRef("octocat", "Hello-World", repo.defaultBranch);
  CHECK_EQ(commit.sha.size(), 40u);
  CHECK(commit.date > 0);
  RepoTree tree(commit.treeSha, c.GetTree("octocat", "Hello-World", commit.treeSha, true));
  RepoTree::Fetch fetch = [&](const std::string& sha) { return c.GetTree("octocat", "Hello-World", sha, false); };
  const TreeNode* readme = tree.Find("README", fetch);
  CHECK(readme != nullptr);
  if (!readme) return;
  std::string body = Fetch([&](const DownloadSink& s) { c.DownloadBlob("octocat", "Hello-World", readme->sha, s); });
  CHECK(body.rfind("Hello World", 0) == 0);
  CHECK_EQ(body.size(), readme->size);
}

TEST("branches and conditional requests") {
  GitHubClient c = Client();
  auto branches = c.Branches("octocat", "Hello-World", 100);
  CHECK(std::any_of(branches.begin(), branches.end(), [](const Ref& r) { return r.name == "master"; }));
  c.GetRepo("octocat", "Hello-World");
  c.GetRepo("octocat", "Hello-World");
  CHECK(c.notModifiedCount() >= 1);
}

TEST("release asset through the redirect, token not forwarded") {
  auto recorder = std::make_shared<Recorder>();
  GitHubClient c(recorder, Token(), "far-github-panel-tests");
  auto releases = c.Releases("cli", "cli", 1);
  CHECK(!releases.empty());
  if (releases.empty()) return;
  const Asset* checksums = nullptr;
  for (const auto& a : releases[0].assets) {
    if (a.name.size() > 13 && a.name.compare(a.name.size() - 13, 13, "checksums.txt") == 0) checksums = &a;
  }
  CHECK(checksums != nullptr);
  if (!checksums) return;
  std::string body = Fetch([&](const DownloadSink& s) { c.DownloadAsset("cli", "cli", checksums->id, s); });
  CHECK_EQ(body.size(), checksums->size);
  CHECK(body.find("gh_") != std::string::npos);
  bool leaked = false;
  for (const auto& s : recorder->seen) {
    if (s.auth && s.url.rfind("https://api.github.com/", 0) != 0) leaked = true;
  }
  CHECK(!leaked);
  CHECK(recorder->seen.size() >= 3);  // releases, asset (302), signed URL
}

TEST("source archive download") {
  GitHubClient c = Client();
  std::string zip = Fetch([&](const DownloadSink& s) { c.DownloadArchive("octocat", "Hello-World", "master", true, s); });
  CHECK(zip.size() > 100);
  CHECK(zip.rfind("PK", 0) == 0);
}

TEST("missing repository is a 404") {
  GitHubClient c = Client();
  try {
    c.GetRepo("octocat", "this-repository-does-not-exist-42");
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK_EQ(e.status(), 404);
  }
}

int main() {
  return test::RunAll();
}

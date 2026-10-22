#include "GitHubClient.hpp"

#include <cstdlib>

#include "Text.hpp"
#include "Url.hpp"

namespace gh {

std::string HttpResponse::header(std::string_view name) const {
  std::string wanted = ToLowerAscii(name);
  for (const auto& h : headers) {
    if (ToLowerAscii(h.name) == wanted) return h.value;
  }
  return {};
}

namespace {

constexpr int kMaxRedirects = 6;

bool IsRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string HostOf(const std::string& url) {
  Url u;
  return ParseUrl(url, u) ? u.host : std::string();
}

std::string Str(const JsonValue& v) {
  return v.isString() ? v.asString() : std::string();
}

Repo ParseRepo(const JsonValue& j) {
  Repo r;
  r.owner = Str(j["owner"]["login"]);
  r.name = Str(j["name"]);
  r.description = Str(j["description"]);
  r.language = Str(j["language"]);
  r.defaultBranch = Str(j["default_branch"]);
  r.htmlUrl = Str(j["html_url"]);
  r.license = Str(j["license"]["spdx_id"]);
  r.isPrivate = j["private"].asBool();
  r.fork = j["fork"].asBool();
  r.archived = j["archived"].asBool();
  r.sizeKb = j["size"].asUInt();
  r.stars = j["stargazers_count"].asUInt();
  r.forks = j["forks_count"].asUInt();
  r.pushedAt = ParseIso8601(Str(j["pushed_at"]));
  return r;
}

Ref ParseRef(const JsonValue& j) {
  return Ref{Str(j["name"]), Str(j["commit"]["sha"])};
}

Release ParseRelease(const JsonValue& j) {
  Release r;
  r.id = j["id"].asUInt();
  r.tag = Str(j["tag_name"]);
  r.name = Str(j["name"]);
  r.htmlUrl = Str(j["html_url"]);
  r.draft = j["draft"].asBool();
  r.prerelease = j["prerelease"].asBool();
  r.publishedAt = ParseIso8601(Str(j["published_at"]));
  if (!r.publishedAt) r.publishedAt = ParseIso8601(Str(j["created_at"]));
  for (const auto& a : j["assets"].items()) {
    Asset asset;
    asset.id = a["id"].asUInt();
    asset.name = Str(a["name"]);
    asset.contentType = Str(a["content_type"]);
    asset.size = a["size"].asUInt();
    asset.downloads = a["download_count"].asUInt();
    asset.updatedAt = ParseIso8601(Str(a["updated_at"]));
    r.assets.push_back(std::move(asset));
  }
  return r;
}

}  // namespace

GitHubClient::GitHubClient(std::shared_ptr<HttpTransport> transport, std::string token, std::string userAgent)
    : transport_(std::move(transport)), token_(std::move(token)), userAgent_(std::move(userAgent)) {}

std::string GitHubClient::Absolute(const std::string& pathOrUrl) const {
  if (pathOrUrl.rfind("https://", 0) == 0 || pathOrUrl.rfind("http://", 0) == 0) return pathOrUrl;
  return std::string(kApi) + pathOrUrl;
}

void GitHubClient::AddHeaders(HttpRequest& request, const std::string& accept, bool auth) const {
  request.headers.push_back({"Accept", accept});
  request.headers.push_back({"User-Agent", userAgent_});
  if (HostOf(request.url) == "api.github.com") request.headers.push_back({"X-GitHub-Api-Version", "2022-11-28"});
  if (auth && !token_.empty()) request.headers.push_back({"Authorization", "Bearer " + token_});
}

void GitHubClient::Record(const HttpResponse& response) {
  std::string limit = response.header("X-RateLimit-Limit");
  if (!limit.empty()) {
    rate_.limit = std::strtoll(limit.c_str(), nullptr, 10);
    rate_.remaining = std::strtoll(response.header("X-RateLimit-Remaining").c_str(), nullptr, 10);
    rate_.reset = std::strtoll(response.header("X-RateLimit-Reset").c_str(), nullptr, 10);
  }
  std::string scopes = response.header("X-OAuth-Scopes");
  if (!scopes.empty()) rate_.scopes = scopes;
}

void GitHubClient::Throw(const HttpResponse& response) {
  Record(response);
  std::string message;
  try {
    JsonValue j = ParseJson(response.body);
    message = Str(j["message"]);
  } catch (const JsonError&) {
  }
  if (message.empty()) message = "HTTP " + std::to_string(response.status);
  bool limited = false;
  int64_t reset = 0;
  if (response.status == 403 || response.status == 429) {
    std::string remaining = response.header("X-RateLimit-Remaining");
    limited = remaining == "0" || !response.header("Retry-After").empty() ||
              ToLowerAscii(message).find("rate limit") != std::string::npos;
    if (limited) reset = std::strtoll(response.header("X-RateLimit-Reset").c_str(), nullptr, 10);
  }
  throw ApiError(response.status, message, limited, reset);
}

JsonValue GitHubClient::GetJson(const std::string& pathOrUrl, std::string* nextLink) {
  std::string url = Absolute(pathOrUrl);
  HttpRequest request;
  request.url = url;
  AddHeaders(request, "application/vnd.github+json", true);
  auto cached = etags_.find(url);
  if (cached != etags_.end()) request.headers.push_back({"If-None-Match", cached->second.etag});
  HttpResponse response = transport_->Send(request);
  std::string body, next;
  if (response.status == 304 && cached != etags_.end()) {
    Record(response);
    ++notModified_;
    body = cached->second.body;
    next = cached->second.next;
  } else if (response.status >= 200 && response.status < 300) {
    Record(response);
    body = std::move(response.body);
    next = NextLink(response.header("Link"));
    std::string etag = response.header("ETag");
    if (!etag.empty()) etags_[url] = Cached{etag, body, next};
  } else {
    Throw(response);
  }
  if (nextLink) *nextLink = next;
  try {
    return ParseJson(body);
  } catch (const JsonError& e) {
    throw ApiError(response.status, std::string("unexpected response: ") + e.what());
  }
}

template <class T>
std::vector<T> GitHubClient::Paged(const std::string& path, size_t limit, bool* more,
                                   const std::function<T(const JsonValue&)>& parse) {
  std::vector<T> out;
  std::string url = path + (path.find('?') == std::string::npos ? "?" : "&") + "per_page=100";
  if (more) *more = false;
  while (!url.empty()) {
    std::string next;
    JsonValue page = GetJson(url, &next);
    for (const auto& item : page.items()) {
      if (out.size() >= limit) {
        if (more) *more = true;
        return out;
      }
      out.push_back(parse(item));
    }
    if (!next.empty() && out.size() >= limit) {
      if (more) *more = true;
      return out;
    }
    url = next;
  }
  return out;
}

std::string GitHubClient::CurrentLogin() {
  return Str(GetJson("/user")["login"]);
}

std::vector<std::string> GitHubClient::MyOrgs() {
  return Paged<std::string>("/user/orgs", 500, nullptr, [](const JsonValue& j) { return Str(j["login"]); });
}

OwnerKind GitHubClient::OwnerType(const std::string& login) {
  JsonValue j = GetJson("/users/" + PercentEncode(login));
  return Str(j["type"]) == "Organization" ? OwnerKind::Organization : OwnerKind::User;
}

std::vector<Repo> GitHubClient::ListRepos(const std::string& owner, OwnerKind kind, size_t limit, bool* more) {
  std::string path;
  switch (kind) {
    case OwnerKind::Self: path = "/user/repos?affiliation=owner&sort=pushed"; break;
    case OwnerKind::Organization: path = "/orgs/" + PercentEncode(owner) + "/repos?sort=pushed&type=all"; break;
    case OwnerKind::User: path = "/users/" + PercentEncode(owner) + "/repos?sort=pushed&type=owner"; break;
  }
  return Paged<Repo>(path, limit, more, ParseRepo);
}

Repo GitHubClient::GetRepo(const std::string& owner, const std::string& repo) {
  return ParseRepo(GetJson("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo)));
}

std::vector<Ref> GitHubClient::Branches(const std::string& owner, const std::string& repo, size_t limit, bool* more) {
  return Paged<Ref>("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/branches", limit, more, ParseRef);
}

std::vector<Ref> GitHubClient::Tags(const std::string& owner, const std::string& repo, size_t limit, bool* more) {
  return Paged<Ref>("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/tags", limit, more, ParseRef);
}

Commit GitHubClient::ResolveRef(const std::string& owner, const std::string& repo, const std::string& ref) {
  std::string base = "/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo);
  // The full commits/{ref} response includes the diff; ask for just the SHA,
  // then read the (immutable, small) git commit object.
  std::string sha;
  {
    std::string url = Absolute(base + "/commits/" + EncodePath(ref));
    HttpRequest request;
    request.url = url;
    AddHeaders(request, "application/vnd.github.sha", true);
    auto cached = etags_.find(url);
    if (cached != etags_.end()) request.headers.push_back({"If-None-Match", cached->second.etag});
    HttpResponse response = transport_->Send(request);
    if (response.status == 304 && cached != etags_.end()) {
      Record(response);
      ++notModified_;
      sha = cached->second.body;
    } else if (response.status >= 200 && response.status < 300) {
      Record(response);
      sha = response.body;
      std::string etag = response.header("ETag");
      if (!etag.empty()) etags_[url] = Cached{etag, sha, {}};
    } else {
      Throw(response);
    }
  }
  while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r' || sha.back() == ' ')) sha.pop_back();
  JsonValue c = GetJson(base + "/git/commits/" + sha);
  Commit commit;
  commit.sha = sha;
  commit.treeSha = Str(c["tree"]["sha"]);
  commit.date = ParseIso8601(Str(c["committer"]["date"]));
  return commit;
}

Tree GitHubClient::GetTree(const std::string& owner, const std::string& repo, const std::string& sha, bool recursive) {
  JsonValue j = GetJson("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/git/trees/" + sha +
                        (recursive ? "?recursive=1" : ""));
  Tree tree;
  tree.sha = Str(j["sha"]);
  tree.truncated = j["truncated"].asBool();
  for (const auto& e : j["tree"].items()) {
    TreeEntry entry;
    entry.path = Str(e["path"]);
    entry.mode = Str(e["mode"]);
    entry.type = Str(e["type"]);
    entry.sha = Str(e["sha"]);
    entry.size = e["size"].asUInt();
    tree.entries.push_back(std::move(entry));
  }
  return tree;
}

std::vector<Release> GitHubClient::Releases(const std::string& owner, const std::string& repo, size_t limit, bool* more) {
  return Paged<Release>("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/releases", limit, more,
                        ParseRelease);
}

void GitHubClient::RefreshRate() {
  HttpRequest request;
  request.url = Absolute("/rate_limit");
  AddHeaders(request, "application/vnd.github+json", true);
  HttpResponse response = transport_->Send(request);
  if (response.status < 200 || response.status >= 300) Throw(response);
  Record(response);
  try {
    JsonValue core = ParseJson(response.body)["resources"]["core"];
    if (core.isObject()) {
      rate_.limit = core["limit"].asInt();
      rate_.remaining = core["remaining"].asInt();
      rate_.reset = core["reset"].asInt();
    }
  } catch (const JsonError&) {
  }
}

void GitHubClient::Download(std::string url, const std::string& accept, const DownloadSink& sink) {
  std::string authHost = HostOf(url);
  for (int hop = 0; hop < kMaxRedirects; ++hop) {
    HttpRequest request;
    request.url = url;
    request.timeoutMs = 60000;
    AddHeaders(request, accept, HostOf(url) == authHost);
    request.onHeaders = [&](const HttpResponse& head) {
      if (sink.onSize) sink.onSize(std::strtoull(head.header("Content-Length").c_str(), nullptr, 10));
      return true;
    };
    request.sink = sink.onData;
    HttpResponse response = transport_->Send(request);
    if (IsRedirect(response.status)) {
      std::string location = response.header("Location");
      if (location.empty()) throw ApiError(response.status, "redirect without a location");
      url = ResolveLocation(url, location);
      continue;
    }
    if (response.status < 200 || response.status >= 300) Throw(response);
    if (HostOf(request.url) == "api.github.com") Record(response);
    return;
  }
  throw ApiError(0, "too many redirects");
}

void GitHubClient::DownloadBlob(const std::string& owner, const std::string& repo, const std::string& sha,
                                const DownloadSink& sink) {
  Download(Absolute("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/git/blobs/" + sha),
           "application/vnd.github.raw", sink);
}

void GitHubClient::DownloadLfs(const std::string& owner, const std::string& repo, const std::string& ref,
                               const std::string& path, const DownloadSink& sink) {
  Download("https://media.githubusercontent.com/media/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/" +
               EncodePath(ref) + "/" + EncodePath(path),
           "*/*", sink);
}

void GitHubClient::DownloadAsset(const std::string& owner, const std::string& repo, uint64_t assetId,
                                 const DownloadSink& sink) {
  Download(Absolute("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + "/releases/assets/" +
                    std::to_string(assetId)),
           "application/octet-stream", sink);
}

void GitHubClient::DownloadArchive(const std::string& owner, const std::string& repo, const std::string& ref,
                                   bool zip, const DownloadSink& sink) {
  Download(Absolute("/repos/" + PercentEncode(owner) + "/" + PercentEncode(repo) + (zip ? "/zipball/" : "/tarball/") +
                    EncodePath(ref)),
           "*/*", sink);
}

bool ParseLfsPointer(const std::string& content, uint64_t* size) {
  static const std::string kHeader = "version https://git-lfs.github.com/spec/v1";
  if (content.size() > 1024 || content.rfind(kHeader, 0) != 0) return false;
  size_t pos = content.find("\nsize ");
  if (pos == std::string::npos) return false;
  if (content.find("\noid sha256:") == std::string::npos) return false;
  if (size) *size = std::strtoull(content.c_str() + pos + 6, nullptr, 10);
  return true;
}

}  // namespace gh

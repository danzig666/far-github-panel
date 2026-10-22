#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Http.hpp"
#include "Json.hpp"

namespace gh {

// Error response from the GitHub API.
class ApiError : public std::runtime_error {
public:
  ApiError(int status, std::string message, bool rateLimited = false, int64_t resetAt = 0)
      : std::runtime_error(message), status_(status), message_(std::move(message)),
        rateLimited_(rateLimited), resetAt_(resetAt) {}
  int status() const { return status_; }
  const std::string& message() const { return message_; }
  bool rateLimited() const { return rateLimited_; }
  int64_t resetAt() const { return resetAt_; }  // Unix time, 0 if unknown

private:
  int status_;
  std::string message_;
  bool rateLimited_;
  int64_t resetAt_;
};

struct RateInfo {
  int64_t limit = -1;
  int64_t remaining = -1;
  int64_t reset = 0;  // Unix time
  std::string scopes; // X-OAuth-Scopes of the last authenticated response
};

enum class OwnerKind { Self, Organization, User };

struct Repo {
  std::string owner, name, description, language, defaultBranch, htmlUrl, license;
  bool isPrivate = false, fork = false, archived = false;
  uint64_t sizeKb = 0, stars = 0, forks = 0;
  int64_t pushedAt = 0;
};

struct Ref {
  std::string name, sha;
};

struct Asset {
  uint64_t id = 0;
  std::string name, contentType;
  uint64_t size = 0, downloads = 0;
  int64_t updatedAt = 0;
};

struct Release {
  uint64_t id = 0;
  std::string tag, name, htmlUrl;
  bool draft = false, prerelease = false;
  int64_t publishedAt = 0;
  std::vector<Asset> assets;
};

struct Commit {
  std::string sha, treeSha;
  int64_t date = 0;
};

struct TreeEntry {
  std::string path, mode, type, sha;  // type: blob, tree, commit (submodule)
  uint64_t size = 0;
};

struct Tree {
  std::string sha;
  bool truncated = false;
  std::vector<TreeEntry> entries;
};

// Called with the total size (from Content-Length; 0 if unknown), then chunks.
struct DownloadSink {
  std::function<void(uint64_t total)> onSize;
  std::function<bool(const char* data, size_t size)> onData;  // false = cancel
};

class GitHubClient {
public:
  static constexpr const char* kApi = "https://api.github.com";

  GitHubClient(std::shared_ptr<HttpTransport> transport, std::string token = {},
               std::string userAgent = "far-github-panel");

  void SetToken(std::string token) { token_ = std::move(token); }
  bool HasToken() const { return !token_.empty(); }
  const RateInfo& Rate() const { return rate_; }

  std::string CurrentLogin();
  std::vector<std::string> MyOrgs();
  OwnerKind OwnerType(const std::string& login);  // Organization or User
  std::vector<Repo> ListRepos(const std::string& owner, OwnerKind kind, size_t limit, bool* more = nullptr);
  Repo GetRepo(const std::string& owner, const std::string& repo);
  std::vector<Ref> Branches(const std::string& owner, const std::string& repo, size_t limit, bool* more = nullptr);
  std::vector<Ref> Tags(const std::string& owner, const std::string& repo, size_t limit, bool* more = nullptr);
  Commit ResolveRef(const std::string& owner, const std::string& repo, const std::string& ref);
  Tree GetTree(const std::string& owner, const std::string& repo, const std::string& sha, bool recursive);
  std::vector<Release> Releases(const std::string& owner, const std::string& repo, size_t limit, bool* more = nullptr);
  void RefreshRate();  // GET /rate_limit

  void DownloadBlob(const std::string& owner, const std::string& repo, const std::string& sha, const DownloadSink& sink);
  void DownloadLfs(const std::string& owner, const std::string& repo, const std::string& ref,
                   const std::string& path, const DownloadSink& sink);
  void DownloadAsset(const std::string& owner, const std::string& repo, uint64_t assetId, const DownloadSink& sink);
  void DownloadArchive(const std::string& owner, const std::string& repo, const std::string& ref, bool zip,
                       const DownloadSink& sink);

  // GET with ETag caching. `path` is relative to the API root or absolute.
  JsonValue GetJson(const std::string& pathOrUrl, std::string* nextLink = nullptr);

  // Number of requests answered from the ETag cache (304), for tests.
  size_t notModifiedCount() const { return notModified_; }
  void ClearCache() { etags_.clear(); }

private:
  struct Cached {
    std::string etag;
    std::string body;
    std::string next;
  };

  std::string Absolute(const std::string& pathOrUrl) const;
  void AddHeaders(HttpRequest& request, const std::string& accept, bool auth) const;
  void Record(const HttpResponse& response);
  [[noreturn]] void Throw(const HttpResponse& response);
  template <class T>
  std::vector<T> Paged(const std::string& path, size_t limit, bool* more,
                       const std::function<T(const JsonValue&)>& parse);
  // GET following redirects; drops Authorization when the host changes.
  void Download(std::string url, const std::string& accept, const DownloadSink& sink);

  std::shared_ptr<HttpTransport> transport_;
  std::string token_;
  std::string userAgent_;
  RateInfo rate_;
  std::map<std::string, Cached> etags_;
  size_t notModified_ = 0;
};

// Git LFS pointer detection: returns true and the real size if `content` is
// a pointer file.
bool ParseLfsPointer(const std::string& content, uint64_t* size = nullptr);

}  // namespace gh

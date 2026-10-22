#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gh {

// Virtual folder names inside a repository.
inline constexpr const char* kBranchesDir = "[branches]";
inline constexpr const char* kTagsDir = "[tags]";
inline constexpr const char* kReleasesDir = "[releases]";

enum class Level { Root, Owner, Tree, Branches, Tags, Releases, Release };
enum class RefKind { Default, Branch, Tag };

// Where a panel is. Every location is a list of path components:
//   []                                   Root
//   [owner]                              Owner
//   [owner, repo, path...]               Tree on the default branch
//   [owner, repo, "[branches]"]          Branches
//   [owner, repo, "[branches]", b, ...]  Tree on branch b
//   [owner, repo, "[tags]", t, ...]      Tags / tree on tag t
//   [owner, repo, "[releases]", tag]     Releases / one release
struct Location {
  Level level = Level::Root;
  std::string owner, repo;
  RefKind refKind = RefKind::Default;
  std::string ref;      // branch or tag name (empty for the default branch)
  std::string path;     // folder inside the tree, '/' separated, "" = root
  std::string release;  // release tag
};

Location Classify(const std::vector<std::string>& comps);

// Ref names may contain '/'. In panel item names it is shown as U+2215 so
// Far does not take it for a path separator.
std::wstring RefToDisplay(const std::wstring& name);
std::wstring RefFromDisplay(const std::wstring& name);

// A github.com URL broken into parts. `rest` is everything after
// /tree/ or /blob/; which prefix of it is the ref is decided by the caller
// against the real branch/tag names (see RefSplits).
struct GitHubUrl {
  enum class Kind { Owner, Repo, Tree, Blob, Releases, Release } kind = Kind::Owner;
  std::string owner, repo, rest, release;
};
bool ParseGitHubUrl(std::string_view url, GitHubUrl& out);

// All ways to split "a/b/c" into (ref, path), longest ref first:
// ("a/b/c",""), ("a/b","c"), ("a","b/c").
std::vector<std::pair<std::string, std::string>> RefSplits(const std::string& rest);

}  // namespace gh

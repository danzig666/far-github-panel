#include "Location.hpp"

#include <algorithm>

#include "Text.hpp"
#include "Url.hpp"

namespace gh {

Location Classify(const std::vector<std::string>& comps) {
  Location loc;
  if (comps.empty()) return loc;
  loc.owner = comps[0];
  if (comps.size() == 1) {
    loc.level = Level::Owner;
    return loc;
  }
  loc.repo = comps[1];
  loc.level = Level::Tree;
  if (comps.size() >= 3) {
    const std::string& third = comps[2];
    if (third == kBranchesDir || third == kTagsDir) {
      bool branches = third == kBranchesDir;
      if (comps.size() == 3) {
        loc.level = branches ? Level::Branches : Level::Tags;
        return loc;
      }
      loc.refKind = branches ? RefKind::Branch : RefKind::Tag;
      loc.ref = comps[3];
      loc.path = JoinPath(comps, 4, comps.size());
      return loc;
    }
    if (third == kReleasesDir) {
      if (comps.size() == 3) {
        loc.level = Level::Releases;
      } else {
        loc.level = Level::Release;
        loc.release = comps[3];
      }
      return loc;
    }
  }
  loc.path = JoinPath(comps, 2, comps.size());
  return loc;
}

std::wstring RefToDisplay(const std::wstring& name) {
  std::wstring out = name;
  std::replace(out.begin(), out.end(), L'/', static_cast<wchar_t>(0x2215));
  return out;
}

std::wstring RefFromDisplay(const std::wstring& name) {
  std::wstring out = name;
  std::replace(out.begin(), out.end(), static_cast<wchar_t>(0x2215), L'/');
  return out;
}

bool ParseGitHubUrl(std::string_view text, GitHubUrl& out) {
  Url url;
  if (!ParseUrl(text, url)) return false;
  if (url.host != "github.com" && url.host != "www.github.com") return false;
  std::string target = url.target.substr(0, url.target.find_first_of("?#"));
  std::vector<std::string> parts = SplitPath(target);
  if (parts.empty()) return false;
  GitHubUrl r;
  r.owner = parts[0];
  if (parts.size() == 1) {
    r.kind = GitHubUrl::Kind::Owner;
    out = r;
    return true;
  }
  r.repo = parts[1];
  if (r.repo.size() > 4 && r.repo.compare(r.repo.size() - 4, 4, ".git") == 0) r.repo.resize(r.repo.size() - 4);
  r.kind = GitHubUrl::Kind::Repo;
  if (parts.size() >= 4 && (parts[2] == "tree" || parts[2] == "blob")) {
    r.kind = parts[2] == "tree" ? GitHubUrl::Kind::Tree : GitHubUrl::Kind::Blob;
    r.rest = JoinPath(parts, 3, parts.size());
  } else if (parts.size() >= 3 && parts[2] == "releases") {
    r.kind = GitHubUrl::Kind::Releases;
    if (parts.size() >= 5 && parts[3] == "tag") {
      r.kind = GitHubUrl::Kind::Release;
      r.release = JoinPath(parts, 4, parts.size());
    }
  }
  out = r;
  return true;
}

std::vector<std::pair<std::string, std::string>> RefSplits(const std::string& rest) {
  std::vector<std::string> parts = SplitPath(rest);
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t n = parts.size(); n >= 1; --n) {
    out.emplace_back(JoinPath(parts, 0, n), JoinPath(parts, n, parts.size()));
  }
  return out;
}

}  // namespace gh

#include "Panel.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <set>

#include "../core/Text.hpp"
#include "../core/Url.hpp"
#include "Config.hpp"
#include "FarApi.hpp"
#include "Guids.hpp"
#include "Progress.hpp"
#include "Updater.hpp"
#include "Version.hpp"
#include "WinHttpTransport.hpp"

using fui::U;
using fui::W;

namespace {

constexpr size_t kListLimit = 3000;
constexpr ULONGLONG kCommitTtlMs = 60 * 1000;
const wchar_t* kTitle = L"GitHub";

std::set<Panel*>& AllPanels() {
  static std::set<Panel*> panels;
  return panels;
}

std::shared_ptr<gh::HttpTransport> Transport() {
  static std::shared_ptr<gh::HttpTransport> transport = std::make_shared<gh::WinHttpTransport>();
  return transport;
}

std::wstring Lower(std::wstring text) {
  for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
  return text;
}

std::wstring Trim(const std::wstring& text) {
  size_t a = text.find_first_not_of(L" \t");
  if (a == std::wstring::npos) return L"";
  size_t b = text.find_last_not_of(L" \t");
  return text.substr(a, b - a + 1);
}

std::string Short(const std::string& sha) {
  return sha.substr(0, 7);
}

std::string RepoKey(const std::string& owner, const std::string& repo) {
  return owner + "/" + repo;
}

std::wstring JoinLocal(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
  return dir + L"\\" + name;
}

std::wstring FullLocalPath(const std::wstring& path) {
  DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!size) return path;
  std::wstring out(size, L'\0');
  out.resize(GetFullPathNameW(path.c_str(), size, out.data(), nullptr));
  return out;
}

std::wstring LastErrorText() {
  DWORD code = GetLastError();
  wchar_t* buffer = nullptr;
  DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring text = len && buffer ? std::wstring(buffer, len) : L"error " + std::to_wstring(code);
  if (buffer) LocalFree(buffer);
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
  return text;
}

[[noreturn]] void ThrowLastError(const std::wstring& what) {
  throw std::runtime_error(U(what + L"\n" + LastErrorText()));
}

bool CreateDirs(const std::wstring& path) {
  std::wstring p = path;
  while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  if (p.empty()) return true;
  DWORD attrs = GetFileAttributesW(fui::LongPath(p).c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 2 && !CreateDirs(p.substr(0, slash))) return false;
  return CreateDirectoryW(fui::LongPath(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

class File {
public:
  explicit File(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
  ~File() { Close(); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  HANDLE get() const { return h_; }
  bool valid() const { return h_ != INVALID_HANDLE_VALUE; }
  void Close() {
    if (valid()) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

private:
  HANDLE h_;
};

// Ref names may contain characters Windows file names cannot.
std::wstring ArchiveName(const std::string& repo, const std::string& ref, bool zip) {
  std::string r = ref;
  std::replace(r.begin(), r.end(), '/', '-');
  return fui::SafeFileName(W(repo + "-" + r + (zip ? ".zip" : ".tar.gz")));
}

}  // namespace

// ---------------------------------------------------------------------------
// Session

Session& Session::Get() {
  static Session session;
  if (!session.client) session.Login();
  return session;
}

void Session::Login() {
  TokenResult r = FindToken();
  client = std::make_shared<gh::GitHubClient>(Transport(), r.token, "far-github-panel/" GHP_VERSION_STRING);
  source = r.source;
  login.clear();
  loginChecked = false;
}

bool Session::FallBack() {
  if (source == LoginSource::Anonymous) return false;
  TokenResult r = FindToken(&source);
  client->SetToken(r.token);
  client->ClearCache();
  source = r.source;
  login.clear();
  loginChecked = false;
  return true;
}

void Session::ForgetRepo(const std::string& owner, const std::string& repo) {
  std::string key = RepoKey(owner, repo);
  repos.erase(key);
  releases.erase(key);
  for (auto it = commits.begin(); it != commits.end();) {
    if (it->first.rfind(key + "@", 0) == 0) {
      it = commits.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// Panel basics

bool Panel::Item::isDir() const {
  switch (kind) {
    case Kind::Owner:
    case Kind::Repo:
    case Kind::Special:
    case Kind::Ref:
    case Kind::Release:
    case Kind::TreeDir:
      return true;
    default:
      return false;
  }
}

Panel::Panel() {
  AllPanels().insert(this);
}

Panel::~Panel() {
  AllPanels().erase(this);
}

Panel* Panel::FromHandle(HANDLE handle) {
  auto* panel = static_cast<Panel*>(handle);
  return AllPanels().count(panel) ? panel : nullptr;
}

Panel* Panel::Active() {
  PanelInfo pi{sizeof(pi)};
  if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi)) return nullptr;
  if (!(pi.Flags & PFLAGS_PLUGIN) || pi.OwnerGuid != PluginGuid) return nullptr;
  return FromHandle(pi.PluginHandle);
}

gh::GitHubClient& Panel::Client() {
  return *Session::Get().client;
}

bool Panel::Run(const std::wstring& title, bool silent, const std::function<void()>& fn) {
  Session& s = Session::Get();
  for (int attempt = 0;; ++attempt) {
    try {
      fn();
      return true;
    } catch (const gh::CancelledError&) {
    } catch (const gh::ApiError& e) {
      if (e.status() == 401 && attempt == 0) {
        LoginSource rejected = s.source;
        if (s.FallBack()) {
          if (!silent)
            fui::ShowError(title, L"GitHub rejected the token from " + SourceName(rejected) + L".\nTrying " +
                                      SourceName(s.source) + L" instead.");
          continue;
        }
      }
      if (!silent) {
        std::wstring text;
        if (e.rateLimited()) {
          text = L"GitHub rate limit reached.";
          if (e.resetAt()) text += L"\nIt resets at " + fui::FormatTime(e.resetAt()) + L".";
          if (s.source == LoginSource::Anonymous)
            text += L"\nYou are not logged in. Log in with \"gh auth login\" for 5,000 requests per hour.";
        } else if (e.status() == 404) {
          text = L"Not found, or your login (" + SourceName(s.source) + L") cannot see it.";
        } else {
          text = W(e.message());
          if (e.status()) text += L" (HTTP " + std::to_wstring(e.status()) + L")";
        }
        fui::ShowError(title, text);
      }
    } catch (const gh::NetError& e) {
      if (!silent) fui::ShowError(title, L"Cannot reach GitHub:\n" + W(e.what()));
    } catch (const std::exception& e) {
      if (!silent) fui::ShowError(title, W(e.what()));
    }
    return false;
  }
}

// ---------------------------------------------------------------------------
// Repository data

const gh::Repo& Panel::RepoInfo(const std::string& owner, const std::string& repo) {
  Session& s = Session::Get();
  std::string key = RepoKey(owner, repo);
  auto it = s.repos.find(key);
  if (it == s.repos.end()) it = s.repos.emplace(key, Client().GetRepo(owner, repo)).first;
  return it->second;
}

std::string Panel::RefName(const gh::Location& loc) {
  if (loc.refKind == gh::RefKind::Default) return RepoInfo(loc.owner, loc.repo).defaultBranch;
  return loc.ref;
}

gh::Commit Panel::CommitFor(const std::string& owner, const std::string& repo, const std::string& ref) {
  Session& s = Session::Get();
  std::string key = RepoKey(owner, repo) + "@" + ref;
  ULONGLONG now = GetTickCount64();
  auto it = s.commits.find(key);
  if (it != s.commits.end() && now - it->second.second < kCommitTtlMs) return it->second.first;
  gh::Commit commit = Client().ResolveRef(owner, repo, ref);
  s.commits[key] = {commit, now};
  return commit;
}

gh::RepoTree::Fetch Panel::Fetcher(const std::string& owner, const std::string& repo) {
  std::shared_ptr<gh::GitHubClient> client = Session::Get().client;
  return [client, owner, repo](const std::string& sha) { return client->GetTree(owner, repo, sha, false); };
}

gh::RepoTree& Panel::TreeFor(const std::string& owner, const std::string& repo, const gh::Commit& commit) {
  Session& s = Session::Get();
  auto it = s.trees.find(commit.treeSha);
  if (it == s.trees.end()) {
    auto tree = std::make_shared<gh::RepoTree>(commit.treeSha, Client().GetTree(owner, repo, commit.treeSha, true));
    it = s.trees.emplace(commit.treeSha, tree).first;
  }
  return *it->second;
}

const std::vector<gh::Release>& Panel::ReleasesFor(const std::string& owner, const std::string& repo) {
  Session& s = Session::Get();
  std::string key = RepoKey(owner, repo);
  auto it = s.releases.find(key);
  if (it == s.releases.end()) it = s.releases.emplace(key, Client().Releases(owner, repo, 1000)).first;
  return it->second;
}

// ---------------------------------------------------------------------------
// Listings

void Panel::BuildTree(const gh::Location& loc, std::vector<Item>& items) {
  const gh::Repo& repo = RepoInfo(loc.owner, loc.repo);
  bool atRoot = loc.refKind == gh::RefKind::Default && loc.path.empty();
  if (atRoot) {
    const char* specials[] = {gh::kBranchesDir, gh::kTagsDir, gh::kReleasesDir};
    const wchar_t* descr[] = {L"all branches", L"all tags", L"releases and their files"};
    for (int i = 0; i < 3; ++i) {
      Item it;
      it.kind = Item::Kind::Special;
      it.name = W(specials[i]);
      it.real = specials[i];
      it.descr = descr[i];
      items.push_back(it);
    }
  }
  std::string ref = loc.refKind == gh::RefKind::Default ? repo.defaultBranch : loc.ref;
  gh::Commit commit;
  try {
    commit = CommitFor(loc.owner, loc.repo, ref);
  } catch (const gh::ApiError& e) {
    if (e.status() == 409 && atRoot) {  // empty repository
      Item it;
      it.name = L"(empty repository)";
      items.push_back(it);
      buildCommit_ = {};
      return;
    }
    throw;
  }
  buildCommit_ = commit;
  gh::RepoTree& tree = TreeFor(loc.owner, loc.repo, commit);
  for (const auto& node : tree.Children(loc.path, Fetcher(loc.owner, loc.repo))) {
    Item it;
    it.node = node;
    it.name = W(node.name);
    it.real = node.name;
    it.time = commit.date;
    if (node.isDir()) {
      it.kind = Item::Kind::TreeDir;
    } else if (node.isSubmodule()) {
      it.kind = Item::Kind::Submodule;
      it.descr = L"submodule at " + W(Short(node.sha));
    } else {
      it.kind = Item::Kind::TreeFile;
      it.size = node.size;
      if (node.isSymlink()) it.descr = L"symlink";
      if (node.isExecutable()) it.descr = L"executable";
    }
    items.push_back(it);
  }
}

std::vector<Panel::Item> Panel::Build(const std::vector<std::string>& comps, const gh::Location& loc) {
  (void)comps;
  Session& s = Session::Get();
  std::vector<Item> items;
  switch (loc.level) {
    case gh::Level::Root: {
      if (Client().HasToken() && !s.loginChecked) {
        s.login = Client().CurrentLogin();
        s.loginChecked = true;
      }
      std::set<std::string> seen;
      auto addOwner = [&](const std::string& login, const wchar_t* descr) {
        if (login.empty() || !seen.insert(gh::ToLowerAscii(login)).second) return;
        Item it;
        it.kind = Item::Kind::Owner;
        it.name = W(login);
        it.real = login;
        it.descr = descr;
        items.push_back(it);
      };
      if (!s.login.empty()) {
        addOwner(s.login, L"you");
        s.ownerKinds[s.login] = gh::OwnerKind::Self;
        for (const auto& org : Client().MyOrgs()) {
          addOwner(org, L"organization");
          s.ownerKinds[org] = gh::OwnerKind::Organization;
        }
      }
      for (const auto& extra : config::ExtraOwners()) addOwner(extra, L"added with F7 (F8 removes it)");
      if (s.source == LoginSource::Anonymous) {
        Item it;
        it.name = L"(not logged in - run \"gh auth login\", or F7 to add an owner)";
        items.push_back(it);
      }
      break;
    }
    case gh::Level::Owner: {
      gh::OwnerKind kind;
      if (!s.login.empty() && gh::ToLowerAscii(s.login) == gh::ToLowerAscii(loc.owner)) {
        kind = gh::OwnerKind::Self;
      } else if (s.ownerKinds.count(loc.owner)) {
        kind = s.ownerKinds[loc.owner];
      } else {
        kind = Client().OwnerType(loc.owner);
        s.ownerKinds[loc.owner] = kind;
      }
      bool more = false;
      for (const auto& r : Client().ListRepos(loc.owner, kind, kListLimit, &more)) {
        s.repos[RepoKey(loc.owner, r.name)] = r;
        Item it;
        it.kind = Item::Kind::Repo;
        it.name = W(r.name);
        it.real = r.name;
        it.ref = r.defaultBranch;
        it.size = r.sizeKb * 1024;
        it.time = r.pushedAt;
        std::wstring d;
        if (r.isPrivate) d += L"private ";
        if (r.fork) d += L"fork ";
        if (r.archived) d += L"archived ";
        if (r.stars) d += L"\x2605" + std::to_wstring(r.stars) + L" ";
        if (!r.language.empty()) d += W(r.language) + L" ";
        if (!d.empty() && !r.description.empty()) d += L"- ";
        d += W(r.description);
        it.descr = d;
        items.push_back(it);
      }
      if (more) {
        Item it;
        it.name = L"(more repositories - open the owner on github.com)";
        items.push_back(it);
      }
      break;
    }
    case gh::Level::Tree:
      BuildTree(loc, items);
      break;
    case gh::Level::Branches:
    case gh::Level::Tags: {
      bool branches = loc.level == gh::Level::Branches;
      std::string def = RepoInfo(loc.owner, loc.repo).defaultBranch;
      bool more = false;
      auto refs = branches ? Client().Branches(loc.owner, loc.repo, kListLimit, &more)
                           : Client().Tags(loc.owner, loc.repo, kListLimit, &more);
      for (const auto& r : refs) {
        Item it;
        it.kind = Item::Kind::Ref;
        it.name = gh::RefToDisplay(W(r.name));
        it.real = r.name;
        it.ref = r.name;
        it.descr = (branches && r.name == def ? L"default branch, " : L"") + W(Short(r.sha));
        items.push_back(it);
      }
      if (more) {
        Item it;
        it.name = L"(more - see github.com)";
        items.push_back(it);
      }
      break;
    }
    case gh::Level::Releases: {
      for (const auto& r : ReleasesFor(loc.owner, loc.repo)) {
        Item it;
        it.kind = Item::Kind::Release;
        it.name = gh::RefToDisplay(W(r.tag));
        it.real = r.tag;
        it.time = r.publishedAt;
        for (const auto& a : r.assets) it.size += a.size;
        std::wstring d = W(r.name);
        if (r.draft) d += L" (draft)";
        if (r.prerelease) d += L" (pre-release)";
        d += L", " + std::to_wstring(r.assets.size()) + L" files";
        it.descr = d;
        items.push_back(it);
      }
      break;
    }
    case gh::Level::Release: {
      const gh::Release* release = nullptr;
      for (const auto& r : ReleasesFor(loc.owner, loc.repo)) {
        if (r.tag == loc.release) release = &r;
      }
      if (!release) throw gh::ApiError(404, "release not found");
      for (const auto& a : release->assets) {
        Item it;
        it.kind = Item::Kind::Asset;
        it.asset = a;
        it.name = W(a.name);
        it.real = a.name;
        it.size = a.size;
        it.time = a.updatedAt;
        it.descr = std::to_wstring(a.downloads) + L" downloads";
        items.push_back(it);
      }
      for (bool zip : {true, false}) {
        Item it;
        it.kind = Item::Kind::Archive;
        it.zip = zip;
        it.ref = release->tag;
        it.name = ArchiveName(loc.repo, release->tag, zip);
        it.real = U(it.name);
        it.time = release->publishedAt;
        it.descr = L"source code";
        items.push_back(it);
      }
      break;
    }
  }
  return items;
}

bool Panel::Load(const std::vector<std::string>& comps, bool silent) {
  std::vector<Item> items;
  gh::Location loc = gh::Classify(comps);
  bool ok = Run(kTitle, silent, [&] {
    buildCommit_ = {};
    items = Build(comps, loc);
  });
  if (!ok) return false;
  comps_ = comps;
  loc_ = loc;
  commit_ = buildCommit_;
  items_ = std::move(items);
  byName_.clear();
  for (size_t i = 0; i < items_.size(); ++i) {
    std::wstring name = items_[i].name;
    for (int k = 2; byName_.count(items_[i].name); ++k) items_[i].name = name + L" ~" + std::to_wstring(k);
    byName_[items_[i].name] = i;
  }
  fresh_ = true;
  // Remember it for next time; not during Alt+F7 searches, which wander
  // through every folder silently.
  if (!silent) config::SetLastLocation(comps_);
  return true;
}

// ---------------------------------------------------------------------------
// Paths and opening

std::wstring Panel::DisplayPath(const std::vector<std::string>& comps) const {
  std::wstring out;
  for (size_t i = 0; i < comps.size(); ++i) {
    std::wstring part = W(comps[i]);
    bool refPart = i == 3 && (comps[2] == gh::kBranchesDir || comps[2] == gh::kTagsDir || comps[2] == gh::kReleasesDir);
    out += L"/" + (refPart ? gh::RefToDisplay(part) : part);
  }
  return out;
}

std::vector<std::string> Panel::ParseTarget(const std::wstring& input) {
  std::wstring target = Trim(input);
  std::vector<std::string> comps;
  if (target.rfind(L"http://", 0) == 0 || target.rfind(L"https://", 0) == 0) {
    gh::GitHubUrl url;
    if (!gh::ParseGitHubUrl(U(target), url)) throw std::runtime_error("not a github.com URL");
    comps.push_back(url.owner);
    if (url.kind == gh::GitHubUrl::Kind::Owner) return comps;
    comps.push_back(url.repo);
    switch (url.kind) {
      case gh::GitHubUrl::Kind::Releases:
        comps.push_back(gh::kReleasesDir);
        break;
      case gh::GitHubUrl::Kind::Release:
        comps.push_back(gh::kReleasesDir);
        comps.push_back(url.release);
        break;
      case gh::GitHubUrl::Kind::Tree:
      case gh::GitHubUrl::Kind::Blob: {
        // Which prefix of the rest is the ref? Try the real branch and tag names.
        std::string def = RepoInfo(url.owner, url.repo).defaultBranch;
        std::set<std::string> branches, tags;
        for (const auto& b : Client().Branches(url.owner, url.repo, kListLimit)) branches.insert(b.name);
        for (const auto& t : Client().Tags(url.owner, url.repo, kListLimit)) tags.insert(t.name);
        auto splits = gh::RefSplits(url.rest);
        std::string ref = splits.empty() ? def : splits.back().first;
        std::string path = splits.empty() ? "" : splits.back().second;
        bool isTag = false;
        for (const auto& [r, p] : splits) {
          if (branches.count(r) || tags.count(r)) {
            ref = r;
            path = p;
            isTag = !branches.count(r);
            break;
          }
        }
        std::vector<std::string> parts = gh::SplitPath(path);
        if (url.kind == gh::GitHubUrl::Kind::Blob && !parts.empty()) parts.pop_back();  // open the folder
        if (ref != def || isTag) {
          comps.push_back(isTag ? gh::kTagsDir : gh::kBranchesDir);
          comps.push_back(ref);
        }
        comps.insert(comps.end(), parts.begin(), parts.end());
        break;
      }
      default:
        break;
    }
    return comps;
  }
  std::replace(target.begin(), target.end(), L'\\', L'/');
  for (const auto& part : gh::SplitPath(U(target))) comps.push_back(U(gh::RefFromDisplay(W(part))));
  return comps;
}

bool Panel::Open(const std::wstring& target) {
  std::wstring t = Trim(target);
  if (t.empty()) {
    // Disk menu, F11 or a bare "gh:": go back to where the panel was last.
    // If that place is gone (repository deleted, branch removed), quietly
    // start at the top instead.
    std::vector<std::string> last = config::LastLocation();
    if (!last.empty() && Load(last, true)) return true;
    return Load({}, false);
  }
  std::vector<std::string> comps;
  if (t != L"/") {
    if (!Run(kTitle, false, [&] { comps = ParseTarget(target); })) comps.clear();
  }
  if (Load(comps, false)) return true;
  return !comps.empty() && Load({}, false);
}

// ---------------------------------------------------------------------------
// Far callbacks: info, listing, navigation

void Panel::GetOpenPanelInfo(OpenPanelInfo* info) {
  Session& s = Session::Get();
  curDirW_ = comps_.empty() ? L"" : DisplayPath(comps_);
  if (comps_.empty()) {
    titleW_ = L" GitHub: " + (s.login.empty() ? std::wstring(L"anonymous") : W(s.login)) +
              (s.source == LoginSource::Anonymous ? L"" : L" (" + SourceName(s.source) + L")") + L" ";
  } else {
    titleW_ = L" GitHub:" + curDirW_;
    if (loc_.level == gh::Level::Tree && !commit_.sha.empty())
      titleW_ += L" [" + W(Short(commit_.sha)) + L", " + fui::FormatTime(commit_.date).substr(0, 10) + L"]";
    titleW_ += L" ";
  }

  infoStrings_.clear();
  infoLines_.clear();
  auto line = [&](const std::wstring& text, const std::wstring& data) {
    infoStrings_.push_back(text);
    infoStrings_.push_back(data);
  };
  if (!loc_.repo.empty()) {
    auto it = s.repos.find(RepoKey(loc_.owner, loc_.repo));
    line(L"Repository", W(loc_.owner + "/" + loc_.repo));
    if (it != s.repos.end()) {
      const gh::Repo& r = it->second;
      if (!r.description.empty()) line(L"Description", W(r.description));
      line(L"Visibility", r.isPrivate ? L"private" : L"public");
      line(L"Default branch", W(r.defaultBranch));
      line(L"Stars / forks", std::to_wstring(r.stars) + L" / " + std::to_wstring(r.forks));
      if (!r.license.empty()) line(L"License", W(r.license));
    }
    if (!commit_.sha.empty()) line(L"Commit", W(Short(commit_.sha)) + L"  " + fui::FormatTime(commit_.date));
  }
  line(L"Login", (s.login.empty() ? std::wstring(L"anonymous") : W(s.login)) + L" (" + SourceName(s.source) + L")");
  const gh::RateInfo& rate = Client().Rate();
  if (rate.limit > 0) line(L"API requests left", std::to_wstring(rate.remaining) + L" of " + std::to_wstring(rate.limit));
  for (size_t i = 0; i + 1 < infoStrings_.size(); i += 2) {
    infoLines_.push_back(InfoPanelLine{infoStrings_[i].c_str(), infoStrings_[i + 1].c_str(), IPLFLAGS_NONE});
  }

  keyLabels_.clear();
  bool root = comps_.empty();
  auto label = [&](WORD vk, DWORD state, const wchar_t* text) {
    keyLabels_.push_back(KeyBarLabel{FarKey{vk, state}, text, text});
  };
  label(VK_F4, 0, L"View");
  label(VK_F6, 0, L"");
  label(VK_F7, 0, root ? L"Add" : L"");
  label(VK_F8, 0, root ? L"Remove" : L"");
  label(VK_F6, SHIFT_PRESSED, L"");
  label(VK_F1, SHIFT_PRESSED, L"Action");
  keyBar_.CountLabels = keyLabels_.size();
  keyBar_.Labels = keyLabels_.data();

  info->StructSize = sizeof(*info);
  info->Flags = OPIF_ADDDOTS | OPIF_SHOWPRESERVECASE | OPIF_SHORTCUT;
  info->HostFile = nullptr;
  info->CurDir = curDirW_.c_str();
  info->Format = L"GitHub";
  info->PanelTitle = titleW_.c_str();
  info->ShortcutData = root ? L"/" : curDirW_.c_str();
  info->InfoLines = infoLines_.data();
  info->InfoLinesNumber = infoLines_.size();
  info->KeyBar = &keyBar_;
}

intptr_t Panel::GetFindData(GetFindDataInfo* info) {
  bool silent = (info->OpMode & (OPM_FIND | OPM_SILENT)) != 0;
  if (!fresh_ && !Load(comps_, silent) && (info->OpMode & OPM_FIND)) return FALSE;
  fresh_ = false;
  auto data = std::make_unique<FindData>();
  auto str = [&](const std::wstring& s) -> const wchar_t* {
    data->strings.push_back(s);
    return data->strings.back().c_str();
  };
  data->items.resize(items_.size());
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& it = items_[i];
    PluginPanelItem& p = data->items[i];
    p = {};
    p.FileName = str(it.name);
    p.Description = it.descr.empty() ? nullptr : str(it.descr);
    p.FileSize = it.size;
    if (it.time) {
      FILETIME ft = fui::UnixToFileTime(it.time);
      p.LastWriteTime = p.CreationTime = p.LastAccessTime = p.ChangeTime = ft;
    }
    uintptr_t attrs = 0;
    if (it.isDir()) attrs |= FILE_ATTRIBUTE_DIRECTORY;
    if (it.kind == Item::Kind::Submodule) attrs |= FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    if (it.kind == Item::Kind::TreeFile && it.node.isSymlink()) attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
    if (!it.name.empty() && it.name[0] == L'.') attrs |= FILE_ATTRIBUTE_HIDDEN;
    attrs |= FILE_ATTRIBUTE_READONLY;
    p.FileAttributes = attrs;
  }
  info->PanelItem = data->items.empty() ? nullptr : data->items.data();
  info->ItemsNumber = data->items.size();
  if (info->PanelItem) findData_[info->PanelItem] = std::move(data);
  return TRUE;
}

void Panel::FreeFindData(const FreeFindDataInfo* info) {
  findData_.erase(info->PanelItem);
}

const Panel::Item* Panel::ItemByName(const std::wstring& name) const {
  auto it = byName_.find(name);
  return it == byName_.end() ? nullptr : &items_[it->second];
}

intptr_t Panel::SetDirectory(const SetDirectoryInfo* info) {
  std::wstring dir = info->Dir ? info->Dir : L"";
  bool silent = (info->OpMode & (OPM_FIND | OPM_SILENT)) != 0;
  if (dir.empty()) return FALSE;
  std::vector<std::string> next = comps_;
  if (dir == L"..") {
    if (next.empty()) return FALSE;
    next.pop_back();
  } else if (dir == L"\\" || dir == L"/") {
    next.clear();
  } else if (dir[0] == L'/' || dir.rfind(L"https://", 0) == 0) {
    if (!Run(kTitle, silent, [&] { next = ParseTarget(dir); })) return FALSE;
  } else if (const Item* it = ItemByName(dir)) {
    if (it->kind == Item::Kind::Submodule) {
      if (!silent)
        fui::ShowInfo(kTitle, L"\"" + it->name + L"\" is a submodule (commit " + W(Short(it->node.sha)) +
                                  L").\nOpen the submodule's own repository to browse it.");
      return FALSE;
    }
    if (!it->isDir()) return FALSE;
    next.push_back(it->real);
  } else {
    std::replace(dir.begin(), dir.end(), L'\\', L'/');
    for (const auto& part : gh::SplitPath(U(dir))) next.push_back(U(gh::RefFromDisplay(W(part))));
  }
  return Load(next, silent) ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// Read-only operations

void Panel::ReadOnly() {
  fui::ShowInfo(kTitle, L"This panel is read-only.\nUse F5 to copy files to your PC.");
}

intptr_t Panel::PutFiles(const PutFilesInfo* info) {
  (void)info;
  ReadOnly();
  return 0;
}

intptr_t Panel::DeleteFiles(const DeleteFilesInfo* info) {
  if (!comps_.empty()) {
    ReadOnly();
    return FALSE;
  }
  std::vector<std::string> owners = config::ExtraOwners();
  std::set<std::string> remove;
  for (size_t i = 0; i < info->ItemsNumber; ++i) {
    if (!info->PanelItem[i].FileName) continue;
    std::string name = U(info->PanelItem[i].FileName);
    if (std::find(owners.begin(), owners.end(), name) != owners.end()) remove.insert(name);
  }
  if (remove.empty()) {
    fui::ShowInfo(kTitle, L"Only owners you added with F7 can be removed.\n"
                          L"Your account and organizations come from GitHub.");
    return FALSE;
  }
  if (!(info->OpMode & OPM_SILENT) &&
      !fui::Confirm(L"Remove", L"Remove " + std::to_wstring(remove.size()) + L" owner(s) from this list?\n"
                                   L"Nothing is changed on GitHub.",
                    L"&Remove")) {
    return FALSE;
  }
  owners.erase(std::remove_if(owners.begin(), owners.end(), [&](const std::string& o) { return remove.count(o) > 0; }),
               owners.end());
  config::SetExtraOwners(owners);
  fresh_ = false;
  return TRUE;
}

intptr_t Panel::MakeDirectory(MakeDirectoryInfo* info) {
  if (!comps_.empty()) {
    ReadOnly();
    return -1;
  }
  std::wstring name;
  if (!fui::Input(L"Add owner", L"GitHub user or organization (or its github.com URL):", name, L"GitHubPanel.Owner"))
    return -1;
  name = Trim(name);
  for (const wchar_t* prefix : {L"https://github.com/", L"http://github.com/", L"github.com/"}) {
    if (Lower(name).rfind(prefix, 0) == 0) name = name.substr(wcslen(prefix));
  }
  while (!name.empty() && name.back() == L'/') name.pop_back();
  if (name.empty() || name.find(L'/') != std::wstring::npos) return -1;
  std::string login = U(name);
  bool ok = Run(L"Add owner", false, [&] {
    Session& s = Session::Get();
    s.ownerKinds[login] = Client().OwnerType(login);
  });
  if (!ok) return -1;
  std::vector<std::string> owners = config::ExtraOwners();
  if (std::find(owners.begin(), owners.end(), login) == owners.end()) owners.push_back(login);
  config::SetExtraOwners(owners);
  fresh_ = false;
  newNameW_ = name;
  info->Name = newNameW_.c_str();
  return 1;
}

// ---------------------------------------------------------------------------
// Downloads

std::vector<const Panel::Item*> Panel::ItemsFrom(const PluginPanelItem* items, size_t count) const {
  std::vector<const Item*> out;
  for (size_t i = 0; i < count; ++i) {
    if (!items[i].FileName || !wcscmp(items[i].FileName, L"..")) continue;
    if (const Item* it = ItemByName(items[i].FileName)) out.push_back(it);
  }
  return out;
}

std::vector<const Panel::Item*> Panel::SelectedItems(bool currentOnly) const {
  std::vector<const Item*> out;
  PanelInfo pi{sizeof(pi)};
  if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi)) return out;
  auto fetch = [&](FILE_CONTROL_COMMANDS cmd, intptr_t index) {
    intptr_t size = PsInfo.PanelControl(PANEL_ACTIVE, cmd, index, nullptr);
    if (size <= 0) return;
    std::vector<std::max_align_t> buffer((static_cast<size_t>(size) + sizeof(std::max_align_t) - 1) /
                                         sizeof(std::max_align_t));
    FarGetPluginPanelItem gpi{sizeof(gpi), static_cast<size_t>(size), reinterpret_cast<PluginPanelItem*>(buffer.data())};
    if (!PsInfo.PanelControl(PANEL_ACTIVE, cmd, index, &gpi) || !gpi.Item->FileName) return;
    if (const Item* it = ItemByName(gpi.Item->FileName)) out.push_back(it);
  };
  if (currentOnly) {
    fetch(FCTL_GETCURRENTPANELITEM, 0);
  } else {
    for (size_t i = 0; i < pi.SelectedItemsNumber; ++i) fetch(FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(i));
  }
  return out;
}

intptr_t Panel::GetFiles(GetFilesInfo* info) {
  if (info->Move) {
    ReadOnly();
    return -1;
  }
  std::vector<const Item*> items = ItemsFrom(info->PanelItem, info->ItemsNumber);
  if (items.empty()) return 0;
  bool quiet = (info->OpMode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;
  std::wstring dest = info->DestPath ? info->DestPath : L"";
  if (!quiet) {
    std::wstring what = items.size() == 1 ? L"\"" + items[0]->name + L"\"" : std::to_wstring(items.size()) + L" items";
    if (!dest.empty() && dest.back() != L'\\') dest += L"\\";
    if (!fui::Input(L"Copy from GitHub", L"Copy " + what + L" to:", dest, L"Copy", FIB_EDITPATH)) return -1;
    dest = Trim(dest);
    if (dest.empty()) return -1;
  }
  destPathW_ = dest;
  info->DestPath = destPathW_.c_str();
  return Download(items, FullLocalPath(dest), info->OpMode);
}

intptr_t Panel::Download(const std::vector<const Item*>& items, const std::wstring& destInput, OPERATION_MODES mode) {
  bool quiet = (mode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;

  struct Job {
    enum class Src { Dir, Blob, Asset, Archive } src = Src::Dir;
    std::vector<std::wstring> rel;  // local path components
    gh::TreeNode node;
    gh::Asset asset;
    std::string owner, repo, ref;
    bool zip = true;
    uint64_t size = 0;
    int64_t time = 0;
    std::wstring remote;
  };
  std::vector<Job> jobs;
  intptr_t result = 1;

  bool ok = Run(L"Copy from GitHub", (mode & OPM_FIND) != 0, [&] {
    Progress progress(L"Copy from GitHub", (mode & OPM_FIND) == 0);
    progress.Status(L"Preparing...");
    std::string treeRef = loc_.level == gh::Level::Tree ? RefName(loc_) : std::string();
    for (const Item* item : items) {
      Job job;
      job.owner = loc_.owner;
      job.repo = loc_.repo;
      job.rel = {fui::SafeFileName(item->name)};
      job.time = item->time;
      job.remote = DisplayPath(comps_) + L"/" + item->name;
      switch (item->kind) {
        case Item::Kind::TreeFile:
          job.src = Job::Src::Blob;
          job.node = item->node;
          job.ref = treeRef;
          job.size = item->size;
          jobs.push_back(job);
          break;
        case Item::Kind::Submodule:
          jobs.push_back(job);
          break;
        case Item::Kind::TreeDir: {
          jobs.push_back(job);
          std::string base = item->node.path + "/";
          gh::RepoTree& tree = TreeFor(loc_.owner, loc_.repo, commit_);
          tree.Walk(item->node.path, Fetcher(loc_.owner, loc_.repo), [&](const gh::TreeNode& n) {
            progress.ThrowIfCancelled();
            Job child = job;
            child.rel = {fui::SafeFileName(item->name)};
            for (const auto& part : gh::SplitPath(n.path.substr(base.size()))) child.rel.push_back(fui::SafeFileName(W(part)));
            child.remote = job.remote + L"/" + W(n.path.substr(base.size()));
            if (n.isDir() || n.isSubmodule()) {
              child.src = Job::Src::Dir;
            } else {
              child.src = Job::Src::Blob;
              child.node = n;
              child.ref = treeRef;
              child.size = n.size;
            }
            jobs.push_back(child);
          });
          break;
        }
        case Item::Kind::Asset:
          job.src = Job::Src::Asset;
          job.asset = item->asset;
          job.size = item->size;
          jobs.push_back(job);
          break;
        case Item::Kind::Archive:
          job.src = Job::Src::Archive;
          job.ref = item->ref;
          job.zip = item->zip;
          jobs.push_back(job);
          break;
        case Item::Kind::Repo:
        case Item::Kind::Ref: {
          job.src = Job::Src::Archive;
          if (item->kind == Item::Kind::Repo) {
            job.repo = item->real;
            job.ref = item->ref.empty() ? RepoInfo(loc_.owner, item->real).defaultBranch : item->ref;
          } else {
            job.ref = item->real;
          }
          job.rel = {ArchiveName(job.repo, job.ref, true)};
          job.time = 0;
          jobs.push_back(job);
          break;
        }
        case Item::Kind::Release: {
          jobs.push_back(job);  // folder named after the tag
          for (const auto& r : ReleasesFor(loc_.owner, loc_.repo)) {
            if (r.tag != item->real) continue;
            for (const auto& a : r.assets) {
              Job asset = job;
              asset.src = Job::Src::Asset;
              asset.asset = a;
              asset.size = a.size;
              asset.time = a.updatedAt;
              asset.rel.push_back(fui::SafeFileName(W(a.name)));
              asset.remote = job.remote + L"/" + W(a.name);
              jobs.push_back(asset);
            }
          }
          break;
        }
        default:
          break;  // owners, [branches] etc.: nothing to copy
      }
    }
    if (jobs.empty()) {
      if (!quiet) fui::ShowInfo(L"Copy from GitHub", L"There is nothing to copy here.\nOpen a repository first.");
      result = 0;
      return;
    }

    // "Copy as": a single file to a path that is not an existing folder.
    std::wstring destDir = destInput;
    DWORD attrs = GetFileAttributesW(fui::LongPath(destInput).c_str());
    bool destIsDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    bool trailing = !destInput.empty() && (destInput.back() == L'\\' || destInput.back() == L'/');
    if (jobs.size() == 1 && jobs[0].src != Job::Src::Dir && !destIsDir && !trailing) {
      size_t slash = destInput.find_last_of(L"\\/");
      if (slash != std::wstring::npos) {
        destDir = destInput.substr(0, slash + 1);
        jobs[0].rel = {destInput.substr(slash + 1)};
      }
    }
    if (!CreateDirs(destDir)) ThrowLastError(L"Cannot create the folder " + destDir);

    uint64_t totalBytes = 0, totalFiles = 0;
    for (const auto& j : jobs) {
      if (j.src != Job::Src::Dir) {
        totalBytes += j.size;
        ++totalFiles;
      }
    }
    progress.SetTotals(totalBytes, totalFiles);
    bool overwriteAll = quiet, skipAll = false;
    gh::GitHubClient& client = Client();

    for (const auto& job : jobs) {
      std::wstring local = destDir;
      for (const auto& part : job.rel) local = JoinLocal(local, part);
      if (job.src == Job::Src::Dir) {
        if (!CreateDirs(local)) ThrowLastError(L"Cannot create the folder " + local);
        continue;
      }
      for (;;) {
        try {
          WIN32_FILE_ATTRIBUTE_DATA fad{};
          if (GetFileAttributesExW(fui::LongPath(local).c_str(), GetFileExInfoStandard, &fad)) {
            if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
              throw std::runtime_error(U(L"A folder with this name already exists:\n" + local));
            if (skipAll) {
              result = 0;
              progress.EndFile();
              break;
            }
            if (!overwriteAll) {
              uint64_t localSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
              intptr_t r = fui::Message(FMSG_WARNING,
                                        {L"File exists", L"The file already exists:", local, L"\x1",
                                         L"New:      " + fui::FormatSize(job.size) +
                                             (job.time ? L"  " + fui::FormatTime(job.time) : L""),
                                         L"Existing: " + fui::FormatSize(localSize)},
                                        {L"&Overwrite", L"&Skip", L"Overwrite &all", L"S&kip all", L"&Cancel"},
                                        MessageGuid);
              progress.Draw(true);
              if (r == 1 || r == 3) {
                if (r == 3) skipAll = true;
                result = 0;
                progress.EndFile();
                break;
              }
              if (r == 2) overwriteAll = true;
              if (r < 0 || r == 4) throw gh::CancelledError();
            }
            SetFileAttributesW(fui::LongPath(local).c_str(), FILE_ATTRIBUTE_NORMAL);
          }

          std::wstring target = fui::LongPath(local);
          std::wstring temp = fui::LongPath(local + L".ghpart");
          File file(CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
          if (!file.valid()) ThrowLastError(L"Cannot create " + local);
          try {
            progress.BeginFile(job.remote, local, job.size);
            auto write = [&](const char* data, size_t size) {
              DWORD written = 0;
              if (!WriteFile(file.get(), data, static_cast<DWORD>(size), &written, nullptr) || written != size)
                ThrowLastError(L"Cannot write " + local);
              progress.AddBytes(size);
              return !progress.CheckCancel();
            };
            gh::DownloadSink sink;
            sink.onData = write;
            for (int attempt = 0;; ++attempt) {
              try {
                LARGE_INTEGER zero{};
                SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN);
                SetEndOfFile(file.get());
                switch (job.src) {
                  case Job::Src::Blob:
                    if (job.size <= 1024) {
                      // Small blobs may be Git LFS pointers: look before writing.
                      std::string content;
                      gh::DownloadSink collect;
                      collect.onData = [&](const char* d, size_t n) {
                        content.append(d, n);
                        return true;
                      };
                      client.DownloadBlob(job.owner, job.repo, job.node.sha, collect);
                      if (gh::ParseLfsPointer(content)) {
                        client.DownloadLfs(job.owner, job.repo, job.ref, job.node.path, sink);
                      } else if (!content.empty()) {
                        write(content.data(), content.size());
                      }
                    } else {
                      client.DownloadBlob(job.owner, job.repo, job.node.sha, sink);
                    }
                    break;
                  case Job::Src::Asset:
                    client.DownloadAsset(job.owner, job.repo, job.asset.id, sink);
                    break;
                  case Job::Src::Archive:
                    client.DownloadArchive(job.owner, job.repo, job.ref, job.zip, sink);
                    break;
                  case Job::Src::Dir:
                    break;
                }
                break;
              } catch (const gh::NetError&) {
                if (attempt >= 2) throw;
                progress.Status(L"Connection lost, retrying...");
                Sleep(1000);
                progress.BeginFile(job.remote, local, job.size);
              }
            }
            if (job.time) {
              FILETIME ft = fui::UnixToFileTime(job.time);
              SetFileTime(file.get(), nullptr, nullptr, &ft);
            }
            file.Close();
            if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
              ThrowLastError(L"Cannot replace " + local);
          } catch (...) {
            file.Close();
            DeleteFileW(temp.c_str());
            throw;
          }
          progress.EndFile();
          break;
        } catch (const gh::CancelledError&) {
          throw;
        } catch (const std::exception& e) {
          if (quiet) throw;
          std::wstring text = W(e.what());
          intptr_t r = fui::Message(FMSG_WARNING, {L"Copy from GitHub", L"Cannot copy", job.remote, text},
                                    {L"&Retry", L"&Skip", L"&Cancel"}, MessageGuid);
          progress.Draw(true);
          if (r == 0) continue;
          if (r == 1) {
            result = 0;
            progress.EndFile();
            break;
          }
          throw gh::CancelledError();
        }
      }
    }
  });
  if (!ok) return -1;
  return result;
}

void Panel::ViewFile(const Item& item) {
  wchar_t temp[MAX_PATH] = {};
  if (!FSF.MkTemp(temp, MAX_PATH, L"GHP") || !CreateDirectoryW(temp, nullptr)) return;
  std::wstring dir = temp;
  if (Download({&item}, dir + L"\\", OPM_VIEW) != 1) {
    RemoveDirectoryW(temp);
    return;
  }
  std::wstring path = JoinLocal(dir, fui::SafeFileName(item.name));
  std::wstring title = W(loc_.owner + "/" + loc_.repo) + L": " + item.name;
  PsInfo.Viewer(path.c_str(), title.c_str(), 0, 0, -1, -1,
                VF_NONMODAL | VF_IMMEDIATERETURN | VF_DELETEONCLOSE | VF_DISABLEHISTORY, CP_DEFAULT);
}

// ---------------------------------------------------------------------------
// Keys and actions

intptr_t Panel::ProcessInput(const ProcessPanelInputInfo* info) {
  const INPUT_RECORD& rec = info->Rec;
  if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) return FALSE;
  WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
  DWORD state = rec.Event.KeyEvent.dwControlKeyState;
  bool shift = (state & SHIFT_PRESSED) != 0;
  bool ctrl = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
  bool alt = (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
  bool plain = !shift && !ctrl && !alt;

  if (shift && !ctrl && !alt && vk == VK_F1) {
    ActionsMenu();
    return TRUE;
  }
  if (ctrl && !shift && !alt && vk == 'R') {
    // Refresh: forget cached refs/releases; unchanged listings are still
    // answered from the ETag cache without using the rate limit.
    if (!loc_.repo.empty()) Session::Get().ForgetRepo(loc_.owner, loc_.repo);
    fresh_ = false;
    return FALSE;
  }
  if (plain && vk == VK_F4) {
    auto current = SelectedItems(true);
    if (current.size() == 1 && (current[0]->kind == Item::Kind::TreeFile || current[0]->kind == Item::Kind::Asset))
      ViewFile(*current[0]);
    return TRUE;
  }
  if ((plain && vk == VK_F6) || (shift && !ctrl && !alt && vk == VK_F6)) {
    ReadOnly();
    return TRUE;
  }
  return FALSE;
}

std::string Panel::WebUrl(const Item* item) const {
  std::string base = "https://github.com";
  const gh::Location& l = loc_;
  auto repoBase = [&] { return base + "/" + gh::PercentEncode(l.owner) + "/" + gh::PercentEncode(l.repo); };
  switch (l.level) {
    case gh::Level::Root:
      return item && item->kind == Item::Kind::Owner ? base + "/" + gh::PercentEncode(item->real) : base;
    case gh::Level::Owner:
      if (item && item->kind == Item::Kind::Repo)
        return base + "/" + gh::PercentEncode(l.owner) + "/" + gh::PercentEncode(item->real);
      return base + "/" + gh::PercentEncode(l.owner);
    case gh::Level::Tree: {
      auto& s = Session::Get();
      auto it = s.repos.find(RepoKey(l.owner, l.repo));
      std::string ref = l.refKind == gh::RefKind::Default ? (it != s.repos.end() ? it->second.defaultBranch : "HEAD") : l.ref;
      if (item && item->kind == Item::Kind::Special) {
        if (item->real == gh::kBranchesDir) return repoBase() + "/branches";
        if (item->real == gh::kTagsDir) return repoBase() + "/tags";
        return repoBase() + "/releases";
      }
      std::string path = l.path;
      if (item && (item->kind == Item::Kind::TreeFile || item->kind == Item::Kind::TreeDir)) path = item->node.path;
      bool file = item && item->kind == Item::Kind::TreeFile;
      if (path.empty() && l.refKind == gh::RefKind::Default) return repoBase();
      return repoBase() + (file ? "/blob/" : "/tree/") + gh::EncodePath(ref) + (path.empty() ? "" : "/" + gh::EncodePath(path));
    }
    case gh::Level::Branches:
    case gh::Level::Tags:
      if (item && item->kind == Item::Kind::Ref) return repoBase() + "/tree/" + gh::EncodePath(item->real);
      return repoBase() + (l.level == gh::Level::Branches ? "/branches" : "/tags");
    case gh::Level::Releases:
      if (item && item->kind == Item::Kind::Release) return repoBase() + "/releases/tag/" + gh::EncodePath(item->real);
      return repoBase() + "/releases";
    case gh::Level::Release:
      if (item && item->kind == Item::Kind::Asset)
        return repoBase() + "/releases/download/" + gh::EncodePath(l.release) + "/" + gh::PercentEncode(item->real);
      return repoBase() + "/releases/tag/" + gh::EncodePath(l.release);
  }
  return base;
}

std::wstring Panel::PassivePanelFolder() const {
  PanelInfo pi{sizeof(pi)};
  if (!PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETPANELINFO, 0, &pi) || (pi.Flags & PFLAGS_PLUGIN)) return L"";
  intptr_t size = PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
  if (size <= 0) return L"";
  std::vector<std::max_align_t> buffer((static_cast<size_t>(size) + sizeof(std::max_align_t) - 1) /
                                       sizeof(std::max_align_t));
  auto dir = reinterpret_cast<FarPanelDirectory*>(buffer.data());
  dir->StructSize = sizeof(FarPanelDirectory);
  if (!PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, size, dir) || !dir->Name) return L"";
  return dir->Name;
}

void Panel::ActionsMenu() {
  auto current = SelectedItems(true);
  const Item* item = current.empty() ? nullptr : current[0];
  std::string url = WebUrl(item);

  // Repository and ref for clone / zip.
  std::string owner = loc_.owner, repo = loc_.repo, ref;
  if (loc_.level == gh::Level::Owner && item && item->kind == Item::Kind::Repo) repo = item->real;
  if (loc_.level == gh::Level::Tree && loc_.refKind != gh::RefKind::Default) ref = loc_.ref;
  if ((loc_.level == gh::Level::Branches || loc_.level == gh::Level::Tags) && item && item->kind == Item::Kind::Ref)
    ref = item->real;

  enum Action { Browser, CopyUrl, Clone, Zip, Rate, Relogin, Update };
  std::vector<std::wstring> labels;
  std::vector<int> actions;
  auto add = [&](const std::wstring& text, int action) {
    labels.push_back(text);
    actions.push_back(action);
  };
  add(L"Open in &browser", Browser);
  add(L"&Copy the URL", CopyUrl);
  if (!repo.empty()) {
    add(L"C&lone with git (puts the command on the command line)", Clone);
    add(L"Download as &ZIP...", Zip);
  }
  add(L"-", -1);
  add(L"&Rate limit and login", Rate);
  add(L"Log in &again (re-read gh / token)", Relogin);
  add(L"-", -1);
  add(L"Check for &updates", Update);

  intptr_t index = fui::Menu(L"GitHub", labels, W(url));
  if (index < 0 || static_cast<size_t>(index) >= actions.size()) return;
  switch (actions[static_cast<size_t>(index)]) {
    case Browser:
      ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case CopyUrl:
      FSF.CopyToClipboard(FCT_STREAM, W(url).c_str());
      break;
    case Clone: {
      std::wstring folder = PassivePanelFolder();
      std::wstring cmd = L"git clone https://github.com/" + W(owner + "/" + repo) + L".git";
      if (!ref.empty()) cmd += L" -b \"" + W(ref) + L"\"";
      if (!folder.empty()) cmd += L" \"" + JoinLocal(folder, W(repo)) + L"\"";
      PsInfo.PanelControl(PANEL_ACTIVE, FCTL_SETCMDLINE, 0, const_cast<wchar_t*>(cmd.c_str()));
      break;
    }
    case Zip: {
      Item zip;
      zip.kind = Item::Kind::Archive;
      zip.zip = true;
      bool ok = Run(kTitle, false, [&] { zip.ref = ref.empty() ? RepoInfo(owner, repo).defaultBranch : ref; });
      if (!ok) return;
      zip.name = ArchiveName(repo, zip.ref, true);
      std::wstring dest = PassivePanelFolder();
      if (!dest.empty() && dest.back() != L'\\') dest += L"\\";
      if (!fui::Input(L"Download as ZIP", L"Save " + zip.name + L" to:", dest, L"Copy", FIB_EDITPATH)) return;
      gh::Location saved = loc_;
      loc_.owner = owner;
      loc_.repo = repo;
      Download({&zip}, FullLocalPath(Trim(dest)), 0);
      loc_ = saved;
      break;
    }
    case Rate: {
      Session& s = Session::Get();
      Run(L"Rate limit", false, [&] { Client().RefreshRate(); });
      const gh::RateInfo& r = Client().Rate();
      std::vector<std::wstring> lines{L"GitHub login",
                                      L"Login:       " + (s.login.empty() ? std::wstring(L"(none)") : W(s.login)),
                                      L"Token from:  " + SourceName(s.source)};
      if (!r.scopes.empty()) lines.push_back(L"Scopes:      " + W(r.scopes));
      lines.push_back(L"\x1");
      lines.push_back(L"API requests left: " + std::to_wstring(r.remaining) + L" of " + std::to_wstring(r.limit));
      if (r.reset) lines.push_back(L"Resets at:   " + fui::FormatTime(r.reset));
      fui::Message(FMSG_LEFTALIGN, lines, {L"OK"}, MessageGuid);
      break;
    }
    case Update:
      updater::CheckForUpdates();
      break;
    case Relogin:
      Session::Get().Login();
      fresh_ = false;
      PsInfo.PanelControl(PANEL_ACTIVE, FCTL_UPDATEPANEL, 1, nullptr);
      PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, nullptr);
      break;
    default:
      break;
  }
}

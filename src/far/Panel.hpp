#pragma once

#include <windows.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../core/GitHubClient.hpp"
#include "../core/Location.hpp"
#include "../core/RepoTree.hpp"
#include "Token.hpp"
#include "plugin.hpp"

class Progress;

// Login and caches shared by all GitHub panels.
struct Session {
  std::shared_ptr<gh::GitHubClient> client;
  TokenSource source = TokenSource::Anonymous;
  std::string login;  // "" when anonymous or not checked yet
  bool loginChecked = false;

  std::map<std::string, gh::Repo> repos;                          // "owner/repo"
  std::map<std::string, gh::OwnerKind> ownerKinds;                // login
  std::map<std::string, std::pair<gh::Commit, ULONGLONG>> commits; // "owner/repo@ref" -> commit, time
  std::map<std::string, std::shared_ptr<gh::RepoTree>> trees;     // tree sha
  std::map<std::string, std::vector<gh::Release>> releases;       // "owner/repo"

  static Session& Get();
  void Login();                         // (re)runs the token chain
  bool FallBack();                      // next token source after a 401
  void ForgetRepo(const std::string& owner, const std::string& repo);
};

class Panel {
public:
  Panel();
  ~Panel();
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;

  static Panel* FromHandle(HANDLE handle);
  static Panel* Active();

  // Opens at the top level, or at a path / github.com URL (command line,
  // shortcuts). Returns false if nothing could be shown.
  bool Open(const std::wstring& target);

  void GetOpenPanelInfo(OpenPanelInfo* info);
  intptr_t GetFindData(GetFindDataInfo* info);
  void FreeFindData(const FreeFindDataInfo* info);
  intptr_t SetDirectory(const SetDirectoryInfo* info);
  intptr_t GetFiles(GetFilesInfo* info);
  intptr_t PutFiles(const PutFilesInfo* info);
  intptr_t DeleteFiles(const DeleteFilesInfo* info);
  intptr_t MakeDirectory(MakeDirectoryInfo* info);
  intptr_t ProcessInput(const ProcessPanelInputInfo* info);

  void ActionsMenu();

private:
  struct Item {
    enum class Kind { Owner, Repo, Special, Ref, Release, Asset, Archive, TreeDir, TreeFile, Submodule, More };
    Kind kind = Kind::More;
    std::wstring name;   // as shown
    std::string real;    // path component / file name it stands for
    uint64_t size = 0;
    int64_t time = 0;
    std::wstring descr;
    gh::TreeNode node;   // TreeDir / TreeFile / Submodule
    gh::Asset asset;     // Asset
    std::string ref;     // Ref, Archive, Repo: ref to download
    bool zip = true;     // Archive
    bool isDir() const;
  };

  struct FindData {
    std::vector<PluginPanelItem> items;
    std::deque<std::wstring> strings;
  };

  gh::GitHubClient& Client();
  bool Run(const std::wstring& title, bool silent, const std::function<void()>& fn);

  // Builds the listing for `comps`; on success makes it current.
  bool Load(const std::vector<std::string>& comps, bool silent);
  std::vector<Item> Build(const std::vector<std::string>& comps, const gh::Location& loc);
  void BuildTree(const gh::Location& loc, std::vector<Item>& items);

  // Repository data with caching.
  const gh::Repo& RepoInfo(const std::string& owner, const std::string& repo);
  std::string RefName(const gh::Location& loc);
  gh::Commit CommitFor(const std::string& owner, const std::string& repo, const std::string& ref);
  gh::RepoTree& TreeFor(const std::string& owner, const std::string& repo, const gh::Commit& commit);
  gh::RepoTree::Fetch Fetcher(const std::string& owner, const std::string& repo);
  const std::vector<gh::Release>& ReleasesFor(const std::string& owner, const std::string& repo);

  std::vector<std::string> ParseTarget(const std::wstring& target);
  std::wstring DisplayPath(const std::vector<std::string>& comps) const;
  std::string WebUrl(const Item* item) const;

  const Item* ItemByName(const std::wstring& name) const;
  std::vector<const Item*> ItemsFrom(const PluginPanelItem* items, size_t count) const;
  std::vector<const Item*> SelectedItems(bool currentOnly) const;
  std::wstring PassivePanelFolder() const;

  // F5 / F3: downloads `items` into `dest`. Returns 1, 0, or -1 (cancelled).
  intptr_t Download(const std::vector<const Item*>& items, const std::wstring& dest, OPERATION_MODES mode);
  void ViewFile(const Item& item);
  void ReadOnly();

  std::vector<std::string> comps_;
  gh::Location loc_;
  std::vector<Item> items_;
  std::map<std::wstring, size_t> byName_;
  bool fresh_ = false;
  gh::Commit commit_;       // current tree level
  gh::Commit buildCommit_;  // set while building a listing

  std::map<PluginPanelItem*, std::unique_ptr<FindData>> findData_;
  std::wstring curDirW_, titleW_, destPathW_, newNameW_;
  std::vector<std::wstring> infoStrings_;
  std::vector<InfoPanelLine> infoLines_;
  std::vector<KeyBarLabel> keyLabels_;
  KeyBarTitles keyBar_{};
};

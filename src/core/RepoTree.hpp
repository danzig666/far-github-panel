#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "GitHubClient.hpp"

namespace gh {

struct TreeNode {
  std::string name, path, type, mode, sha;  // type: blob, tree, commit
  uint64_t size = 0;

  bool isDir() const { return type == "tree"; }
  bool isSubmodule() const { return type == "commit"; }
  bool isSymlink() const { return mode == "120000"; }
  bool isExecutable() const { return mode == "100755"; }
};

// A repository tree at one commit, as a folder map. Built from one recursive
// trees response; if GitHub truncated it, folders are loaded one at a time
// through `fetch` when first opened.
class RepoTree {
public:
  using Fetch = std::function<Tree(const std::string& treeSha)>;  // non-recursive

  RepoTree(std::string rootSha, const Tree& recursive);

  // Children of `dir` ("" is the root), loading it if needed. Throws
  // ApiError(404) if `dir` is not a folder in this tree.
  const std::vector<TreeNode>& Children(const std::string& dir, const Fetch& fetch);

  // The node at `path`, or nullptr. Loads parent folders as needed.
  const TreeNode* Find(const std::string& path, const Fetch& fetch);

  // Depth-first walk of everything under `dir` (folders before contents).
  void Walk(const std::string& dir, const Fetch& fetch, const std::function<void(const TreeNode&)>& visit);

  bool truncated() const { return truncated_; }
  const std::string& rootSha() const { return rootSha_; }

private:
  void AddFolder(const std::string& dir, const Tree& flat);

  std::string rootSha_;
  bool truncated_ = false;
  std::map<std::string, std::vector<TreeNode>> dirs_;
};

std::string ParentPath(const std::string& path);
std::string LeafName(const std::string& path);

}  // namespace gh

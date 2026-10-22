#include "RepoTree.hpp"

namespace gh {

std::string ParentPath(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string LeafName(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

namespace {
TreeNode MakeNode(const TreeEntry& e, const std::string& fullPath) {
  TreeNode n;
  n.path = fullPath;
  n.name = LeafName(fullPath);
  n.type = e.type;
  n.mode = e.mode;
  n.sha = e.sha;
  n.size = e.size;
  return n;
}
}  // namespace

RepoTree::RepoTree(std::string rootSha, const Tree& recursive) : rootSha_(std::move(rootSha)) {
  if (recursive.truncated) {
    // A truncated pre-order listing can miss entries of any folder, so
    // trust none of it; folders are fetched one by one instead.
    truncated_ = true;
    return;
  }
  dirs_[""];
  for (const auto& e : recursive.entries) {
    TreeNode node = MakeNode(e, e.path);
    if (node.isDir()) dirs_[node.path];
    dirs_[ParentPath(e.path)].push_back(std::move(node));
  }
}

void RepoTree::AddFolder(const std::string& dir, const Tree& flat) {
  auto& children = dirs_[dir];
  children.clear();
  for (const auto& e : flat.entries) {
    children.push_back(MakeNode(e, dir.empty() ? e.path : dir + "/" + e.path));
  }
}

const std::vector<TreeNode>& RepoTree::Children(const std::string& dir, const Fetch& fetch) {
  auto it = dirs_.find(dir);
  if (it != dirs_.end()) return it->second;
  std::string sha;
  if (dir.empty()) {
    sha = rootSha_;
  } else {
    const TreeNode* node = Find(dir, fetch);
    if (!node || !node->isDir()) throw ApiError(404, "folder not found: " + dir);
    sha = node->sha;
  }
  AddFolder(dir, fetch(sha));
  return dirs_[dir];
}

const TreeNode* RepoTree::Find(const std::string& path, const Fetch& fetch) {
  if (path.empty()) return nullptr;
  const auto& siblings = Children(ParentPath(path), fetch);
  std::string leaf = LeafName(path);
  for (const auto& n : siblings) {
    if (n.name == leaf) return &n;
  }
  return nullptr;
}

void RepoTree::Walk(const std::string& dir, const Fetch& fetch, const std::function<void(const TreeNode&)>& visit) {
  // Copy: loading subfolders may rehash dirs_, but vectors stay put in a
  // std::map, and copying keeps this simple and safe.
  std::vector<TreeNode> children = Children(dir, fetch);
  for (const auto& n : children) {
    visit(n);
    if (n.isDir()) Walk(n.path, fetch, visit);
  }
}

}  // namespace gh

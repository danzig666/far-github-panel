#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gh {

struct Url {
  bool https = true;
  std::string host;
  uint16_t port = 443;
  std::string target = "/";  // path + query
};

bool ParseUrl(std::string_view text, Url& out);

// Resolves a Location header against the URL that returned it.
std::string ResolveLocation(std::string_view base, std::string_view location);

// The rel="next" URL from a Link header, or empty.
std::string NextLink(std::string_view linkHeader);

// Percent-encodes each segment of a slash-separated path, keeping the slashes.
std::string EncodePath(std::string_view path);

// "a/b/c" -> {"a","b","c"}; empty segments are dropped.
std::vector<std::string> SplitPath(std::string_view path, char sep = '/');
std::string JoinPath(const std::vector<std::string>& parts, size_t begin, size_t end, char sep = '/');

}  // namespace gh

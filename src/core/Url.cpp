#include "Url.hpp"

#include <cstdlib>

#include "Text.hpp"

namespace gh {

bool ParseUrl(std::string_view text, Url& out) {
  Url url;
  if (text.rfind("https://", 0) == 0) {
    url.https = true;
    url.port = 443;
    text.remove_prefix(8);
  } else if (text.rfind("http://", 0) == 0) {
    url.https = false;
    url.port = 80;
    text.remove_prefix(7);
  } else {
    return false;
  }
  size_t slash = text.find_first_of("/?");
  std::string_view authority = text.substr(0, slash);
  url.target = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
  if (!url.target.empty() && url.target[0] == '?') url.target = "/" + url.target;
  size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
    long port = std::strtol(std::string(authority.substr(colon + 1)).c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    url.port = static_cast<uint16_t>(port);
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return false;
  url.host = ToLowerAscii(authority);
  out = std::move(url);
  return true;
}

std::string ResolveLocation(std::string_view base, std::string_view location) {
  if (location.rfind("https://", 0) == 0 || location.rfind("http://", 0) == 0) return std::string(location);
  Url b;
  if (!ParseUrl(base, b)) return std::string(location);
  std::string prefix = (b.https ? "https://" : "http://") + b.host;
  if ((b.https && b.port != 443) || (!b.https && b.port != 80)) prefix += ":" + std::to_string(b.port);
  if (location.rfind("//", 0) == 0) return (b.https ? "https:" : "http:") + std::string(location);
  if (!location.empty() && location[0] == '/') return prefix + std::string(location);
  std::string path = b.target.substr(0, b.target.find('?'));
  path = path.substr(0, path.rfind('/') + 1);
  return prefix + path + std::string(location);
}

std::string NextLink(std::string_view header) {
  // <https://api.github.com/...?page=2>; rel="next", <...>; rel="last"
  size_t pos = 0;
  while (pos < header.size()) {
    size_t lt = header.find('<', pos);
    if (lt == std::string_view::npos) break;
    size_t gt = header.find('>', lt);
    if (gt == std::string_view::npos) break;
    size_t end = header.find(',', gt);
    std::string_view params = header.substr(gt + 1, end == std::string_view::npos ? std::string_view::npos : end - gt - 1);
    if (params.find("rel=\"next\"") != std::string_view::npos) return std::string(header.substr(lt + 1, gt - lt - 1));
    if (end == std::string_view::npos) break;
    pos = end + 1;
  }
  return {};
}

std::string EncodePath(std::string_view path) {
  std::string out;
  size_t start = 0;
  for (;;) {
    size_t slash = path.find('/', start);
    out += PercentEncode(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
    if (slash == std::string_view::npos) break;
    out.push_back('/');
    start = slash + 1;
  }
  return out;
}

std::vector<std::string> SplitPath(std::string_view path, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(sep, start);
    std::string_view part = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!part.empty()) parts.emplace_back(part);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

std::string JoinPath(const std::vector<std::string>& parts, size_t begin, size_t end, char sep) {
  std::string out;
  for (size_t i = begin; i < end && i < parts.size(); ++i) {
    if (!out.empty()) out.push_back(sep);
    out += parts[i];
  }
  return out;
}

}  // namespace gh

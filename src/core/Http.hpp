#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gh {

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpResponse {
  int status = 0;
  std::vector<HttpHeader> headers;
  std::string body;  // filled unless a sink consumed a 2xx body

  // Case-insensitive lookup; empty if missing.
  std::string header(std::string_view name) const;
};

struct HttpRequest {
  std::string method = "GET";
  std::string url;  // absolute: https://host[:port]/path?query
  std::vector<HttpHeader> headers;
  std::string body;

  // Called once the status and headers of a 2xx response are known;
  // returning false aborts with CancelledError.
  std::function<bool(const HttpResponse& head)> onHeaders;

  // Receives the body of a 2xx response in chunks instead of collecting it.
  // Returning false aborts with CancelledError. Other responses are always
  // collected into HttpResponse::body.
  std::function<bool(const char* data, size_t size)> sink;

  int timeoutMs = 30000;
};

// Transport-level failure: DNS, connect, TLS, reset, timeout.
class NetError : public std::runtime_error {
public:
  NetError(const std::string& message, unsigned long code = 0)
      : std::runtime_error(message), code_(code) {}
  unsigned long code() const { return code_; }

private:
  unsigned long code_;
};

// The user (or a callback) aborted the operation.
class CancelledError : public std::runtime_error {
public:
  CancelledError() : std::runtime_error("cancelled") {}
};

// Moves bytes. Must NOT follow redirects: the GitHub client does that itself
// so it can drop the Authorization header when the host changes.
class HttpTransport {
public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse Send(HttpRequest& request) = 0;
};

}  // namespace gh

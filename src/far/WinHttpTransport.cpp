#include "WinHttpTransport.hpp"

#include <vector>

#include "../core/Text.hpp"
#include "../core/Url.hpp"

namespace gh {

namespace {

class Handle {
public:
  explicit Handle(HINTERNET h = nullptr) : h_(h) {}
  ~Handle() {
    if (h_) WinHttpCloseHandle(h_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HINTERNET get() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

private:
  HINTERNET h_;
};

std::string ErrorText(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the host name could not be resolved";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure (TLS) connection failed";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "invalid server response";
    default: break;
  }
  wchar_t* buffer = nullptr;
  DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buffer), 0,
                             nullptr);
  std::string text;
  if (len && buffer) {
    text = WideToUtf8(std::wstring_view(buffer, len));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  }
  if (buffer) LocalFree(buffer);
  if (text.empty()) text = "network error " + std::to_string(code);
  return text;
}

[[noreturn]] void Fail(const char* what) {
  DWORD code = GetLastError();
  throw NetError(std::string(what) + ": " + ErrorText(code), code);
}

}  // namespace

WinHttpTransport::WinHttpTransport() {
  session_ = WinHttpOpen(L"far-github-panel", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_) {
    // Before Windows 8.1: fall back to the WinHTTP proxy configuration.
    session_ = WinHttpOpen(L"far-github-panel", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (session_) {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
  }
}

WinHttpTransport::~WinHttpTransport() {
  if (session_) WinHttpCloseHandle(session_);
}

HttpResponse WinHttpTransport::Send(HttpRequest& request) {
  if (!session_) Fail("WinHttpOpen");
  Url url;
  if (!ParseUrl(request.url, url)) throw NetError("bad URL: " + request.url);
  std::wstring host = Utf8ToWide(url.host);
  Handle connect(WinHttpConnect(session_, host.c_str(), url.port, 0));
  if (!connect) Fail("connect");

  std::wstring method = Utf8ToWide(request.method);
  std::wstring target = Utf8ToWide(url.target);
  Handle req(WinHttpOpenRequest(connect.get(), method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0));
  if (!req) Fail("open request");
  int timeout = request.timeoutMs > 0 ? request.timeoutMs : 30000;
  WinHttpSetTimeouts(req.get(), 10000, 10000, timeout, timeout);
  DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
  WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));

  std::wstring headers;
  for (const auto& h : request.headers) headers += Utf8ToWide(h.name) + L": " + Utf8ToWide(h.value) + L"\r\n";
  DWORD size = static_cast<DWORD>(request.body.size());
  if (!WinHttpSendRequest(req.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : static_cast<DWORD>(-1L),
                          size ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA, size, size, 0)) {
    Fail("send request");
  }
  if (!WinHttpReceiveResponse(req.get(), nullptr)) Fail("receive response");

  HttpResponse response;
  DWORD status = 0, statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    Fail("read status");
  }
  response.status = static_cast<int>(status);

  DWORD rawSize = 0;
  WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                      &rawSize, WINHTTP_NO_HEADER_INDEX);
  if (rawSize) {
    std::wstring raw(rawSize / sizeof(wchar_t) + 1, L'\0');
    if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                            &rawSize, WINHTTP_NO_HEADER_INDEX)) {
      raw.resize(rawSize / sizeof(wchar_t));
      size_t start = raw.find(L"\r\n");  // skip the status line
      while (start != std::wstring::npos) {
        start += 2;
        size_t end = raw.find(L"\r\n", start);
        std::wstring line = raw.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos) {
          std::wstring value = line.substr(colon + 1);
          while (!value.empty() && value.front() == L' ') value.erase(0, 1);
          response.headers.push_back({WideToUtf8(line.substr(0, colon)), WideToUtf8(value)});
        }
        start = end;
      }
    }
  }

  bool ok = response.status >= 200 && response.status < 300;
  if (ok && request.onHeaders && !request.onHeaders(response)) throw CancelledError();
  bool streaming = ok && request.sink;

  std::vector<char> buffer(256 * 1024);
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(req.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) Fail("receive data");
    if (read == 0) break;
    if (streaming) {
      if (!request.sink(buffer.data(), read)) throw CancelledError();
    } else {
      response.body.append(buffer.data(), read);
    }
  }
  return response;
}

}  // namespace gh

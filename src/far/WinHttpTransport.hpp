#pragma once

#include <windows.h>
#include <winhttp.h>

#include "../core/Http.hpp"

namespace gh {

// Synchronous WinHTTP transport. Uses the system proxy settings, TLS for
// https URLs, and never follows redirects (the client does, see Http.hpp).
class WinHttpTransport : public HttpTransport {
public:
  WinHttpTransport();
  ~WinHttpTransport() override;
  WinHttpTransport(const WinHttpTransport&) = delete;
  WinHttpTransport& operator=(const WinHttpTransport&) = delete;

  HttpResponse Send(HttpRequest& request) override;

private:
  HINTERNET session_ = nullptr;
};

}  // namespace gh

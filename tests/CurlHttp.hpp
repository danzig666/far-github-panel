#pragma once

#include "../src/core/Http.hpp"

namespace gh {

// libcurl transport for the Linux integration tests. Never follows redirects.
class CurlHttpTransport : public HttpTransport {
public:
  CurlHttpTransport();
  ~CurlHttpTransport() override;
  HttpResponse Send(HttpRequest& request) override;
};

}  // namespace gh

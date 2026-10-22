#include "CurlHttp.hpp"

#include <curl/curl.h>

#include <exception>
#include <string>

namespace gh {

namespace {

struct Context {
  HttpRequest* request;
  HttpResponse response;
  bool headersDone = false;
  bool streaming = false;
  std::exception_ptr error;
  CURL* curl;
};

void FinishHeaders(Context& ctx) {
  if (ctx.headersDone) return;
  ctx.headersDone = true;
  long status = 0;
  curl_easy_getinfo(ctx.curl, CURLINFO_RESPONSE_CODE, &status);
  ctx.response.status = static_cast<int>(status);
  bool ok = status >= 200 && status < 300;
  if (ok && ctx.request->onHeaders && !ctx.request->onHeaders(ctx.response)) throw CancelledError();
  ctx.streaming = ok && ctx.request->sink;
}

size_t OnHeader(char* data, size_t size, size_t count, void* user) {
  auto& ctx = *static_cast<Context*>(user);
  std::string line(data, size * count);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (line.rfind("HTTP/", 0) == 0) {
    ctx.response.headers.clear();  // a new response (e.g. after 100 Continue)
    return size * count;
  }
  size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
    ctx.response.headers.push_back({line.substr(0, colon), value});
  }
  return size * count;
}

size_t OnData(char* data, size_t size, size_t count, void* user) {
  auto& ctx = *static_cast<Context*>(user);
  size_t n = size * count;
  try {
    FinishHeaders(ctx);
    if (ctx.streaming) {
      if (!ctx.request->sink(data, n)) throw CancelledError();
    } else {
      ctx.response.body.append(data, n);
    }
  } catch (...) {
    ctx.error = std::current_exception();
    return 0;  // aborts the transfer
  }
  return n;
}

}  // namespace

CurlHttpTransport::CurlHttpTransport() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlHttpTransport::~CurlHttpTransport() {
  curl_global_cleanup();
}

HttpResponse CurlHttpTransport::Send(HttpRequest& request) {
  CURL* curl = curl_easy_init();
  if (!curl) throw NetError("curl_easy_init failed");
  Context ctx{&request, {}, false, false, nullptr, curl};
  curl_slist* headers = nullptr;
  for (const auto& h : request.headers) headers = curl_slist_append(headers, (h.name + ": " + h.value).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnData);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeoutMs) * 4);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  if (!request.body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
  }
  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  if (ctx.error) {
    curl_easy_cleanup(curl);
    std::rethrow_exception(ctx.error);
  }
  if (rc != CURLE_OK) {
    std::string message = curl_easy_strerror(rc);
    curl_easy_cleanup(curl);
    throw NetError(message, static_cast<unsigned long>(rc));
  }
  try {
    FinishHeaders(ctx);  // bodiless responses
  } catch (...) {
    curl_easy_cleanup(curl);
    throw;
  }
  curl_easy_cleanup(curl);
  return ctx.response;
}

}  // namespace gh

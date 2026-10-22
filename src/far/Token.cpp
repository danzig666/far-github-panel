#include "Token.hpp"

#include <windows.h>
#include <wincrypt.h>

#include "../core/Text.hpp"
#include "Config.hpp"

namespace {

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t a = s.find_first_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a);
}

std::string Env(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring value(size, L'\0');
  DWORD len = GetEnvironmentVariableW(name, value.data(), size);
  value.resize(len);
  return Trim(gh::WideToUtf8(value));
}

// Finds gh.exe on PATH. Deliberately not CreateProcess's own search, which
// would also run a gh.exe from the current (possibly untrusted) folder.
std::wstring FindGhExe() {
  DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (!size) return {};
  std::wstring path(size, L'\0');
  path.resize(GetEnvironmentVariableW(L"PATH", path.data(), size));
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(L';', start);
    std::wstring dir = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
    if (!dir.empty() && dir != L".") {
      if (dir.back() != L'\\') dir += L'\\';
      std::wstring candidate = dir + L"gh.exe";
      DWORD attrs = GetFileAttributesW(candidate.c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
    }
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return {};
}

}  // namespace

std::string GhCliToken(std::wstring* problem) {
  std::wstring exe = FindGhExe();
  if (exe.empty()) {
    if (problem) *problem = L"gh is not on PATH";
    return {};
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return {};
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = writePipe;
  si.hStdError = nul;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe + L"\" auth token --hostname github.com";
  BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                &si, &pi);
  CloseHandle(writePipe);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!started) {
    CloseHandle(readPipe);
    if (problem) *problem = L"could not start gh";
    return {};
  }
  std::string output;
  ULONGLONG deadline = GetTickCount64() + 10000;
  bool exited = false;
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail) {
      char buf[512];
      DWORD read = 0;
      if (ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) && read) output.append(buf, read);
      continue;
    }
    if (exited) break;
    exited = WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0;
    if (!exited && GetTickCount64() > deadline) {
      TerminateProcess(pi.hProcess, 1);
      if (problem) *problem = L"gh did not answer";
      output.clear();
      break;
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(readPipe);
  std::string token = Trim(output);
  if (code != 0 || token.empty() || token.find(' ') != std::string::npos) {
    if (problem && problem->empty()) *problem = L"gh is not logged in (run: gh auth login)";
    return {};
  }
  return token;
}

TokenResult FindToken(const TokenSource* after) {
  static const TokenSource kOrder[] = {TokenSource::Saved, TokenSource::EnvGhToken, TokenSource::EnvGithubToken,
                                       TokenSource::GhCli};
  bool started = after == nullptr;
  for (TokenSource s : kOrder) {
    if (!started) {
      if (s == *after) started = true;
      continue;
    }
    std::string token;
    switch (s) {
      case TokenSource::Saved: token = config::SavedToken(); break;
      case TokenSource::EnvGhToken: token = config::Opt().useEnv ? Env(L"GH_TOKEN") : ""; break;
      case TokenSource::EnvGithubToken: token = config::Opt().useEnv ? Env(L"GITHUB_TOKEN") : ""; break;
      case TokenSource::GhCli: token = config::Opt().useGhCli ? GhCliToken() : ""; break;
      case TokenSource::Anonymous: break;
    }
    if (!token.empty()) return TokenResult{token, s};
  }
  return TokenResult{};
}

std::wstring SourceName(TokenSource source) {
  switch (source) {
    case TokenSource::Saved: return L"saved token";
    case TokenSource::EnvGhToken: return L"GH_TOKEN";
    case TokenSource::EnvGithubToken: return L"GITHUB_TOKEN";
    case TokenSource::GhCli: return L"gh";
    case TokenSource::Anonymous: return L"anonymous";
  }
  return L"";
}

std::vector<uint8_t> ProtectString(const std::string& text) {
  DATA_BLOB in{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))};
  DATA_BLOB out{};
  std::vector<uint8_t> result;
  if (CryptProtectData(&in, L"far-github-panel", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    result.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
  }
  return result;
}

std::string UnprotectString(const std::vector<uint8_t>& blob) {
  if (blob.empty()) return {};
  DATA_BLOB in{static_cast<DWORD>(blob.size()), const_cast<BYTE*>(blob.data())};
  DATA_BLOB out{};
  std::string result;
  if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    result.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
  }
  return result;
}

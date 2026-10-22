#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Where the GitHub token came from, in the order they are tried.
enum class LoginSource { Saved, EnvGhToken, EnvGithubToken, GhCli, Anonymous };

struct TokenResult {
  std::string token;  // empty for Anonymous
  LoginSource source = LoginSource::Anonymous;
};

// Tries the sources in order. With `after`, starts at the source after it
// (to fall back when GitHub rejected a token); nullptr starts at the top.
TokenResult FindToken(const LoginSource* after = nullptr);

std::wstring SourceName(LoginSource source);

// Runs `gh auth token --hostname github.com` (gh.exe found on PATH only,
// never in the current folder). Returns an empty string if gh is missing,
// not logged in, or does not answer within 10 seconds.
std::string GhCliToken(std::wstring* problem = nullptr);

// DPAPI (current user) wrappers for the saved token.
std::vector<uint8_t> ProtectString(const std::string& text);
std::string UnprotectString(const std::vector<uint8_t>& blob);

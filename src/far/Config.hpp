#pragma once

#include <string>
#include <vector>

struct Options {
  bool addToDiskMenu = true;
  bool useEnv = true;    // GH_TOKEN / GITHUB_TOKEN
  bool useGhCli = true;  // gh auth token
  std::wstring prefix = L"gh";
};

namespace config {

Options& Opt();
void LoadOptions();
void SaveOptions();

// Token typed into the configuration dialog, DPAPI-encrypted at rest.
std::string SavedToken();
bool HasSavedToken();
void SetSavedToken(const std::string& token);  // empty removes it

// Owners added with F7 on the top level (logins, UTF-8).
std::vector<std::string> ExtraOwners();
void SetExtraOwners(const std::vector<std::string>& owners);

// F9 > Options > Plugins configuration. Returns true if the login settings changed.
bool ConfigureDialog();

}  // namespace config

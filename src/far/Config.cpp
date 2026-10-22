#include "Config.hpp"

#include "FarApi.hpp"
#include "Guids.hpp"
#include "Token.hpp"

namespace config {

namespace {

class Settings {
public:
  Settings() {
    FarSettingsCreate create{sizeof(create), PluginGuid, INVALID_HANDLE_VALUE};
    if (PsInfo.SettingsControl(INVALID_HANDLE_VALUE, SCTL_CREATE, PSL_ROAMING, &create)) handle_ = create.Handle;
  }
  ~Settings() {
    if (handle_ != INVALID_HANDLE_VALUE) PsInfo.SettingsControl(handle_, SCTL_FREE, 0, nullptr);
  }
  Settings(const Settings&) = delete;
  Settings& operator=(const Settings&) = delete;

  bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }

  std::wstring GetString(size_t root, const wchar_t* name, const std::wstring& fallback) {
    FarSettingsItem item{sizeof(item), root, name, FST_STRING};
    if (ok() && PsInfo.SettingsControl(handle_, SCTL_GET, 0, &item) && item.String) return item.String;
    return fallback;
  }
  unsigned long long GetNumber(size_t root, const wchar_t* name, unsigned long long fallback) {
    FarSettingsItem item{sizeof(item), root, name, FST_QWORD};
    if (ok() && PsInfo.SettingsControl(handle_, SCTL_GET, 0, &item)) return item.Number;
    return fallback;
  }
  std::vector<uint8_t> GetData(size_t root, const wchar_t* name) {
    FarSettingsItem item{sizeof(item), root, name, FST_DATA};
    if (ok() && PsInfo.SettingsControl(handle_, SCTL_GET, 0, &item) && item.Data.Data) {
      auto p = static_cast<const uint8_t*>(item.Data.Data);
      return std::vector<uint8_t>(p, p + item.Data.Size);
    }
    return {};
  }
  void SetString(size_t root, const wchar_t* name, const std::wstring& value) {
    FarSettingsItem item{sizeof(item), root, name, FST_STRING};
    item.String = value.c_str();
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_SET, 0, &item);
  }
  void SetNumber(size_t root, const wchar_t* name, unsigned long long value) {
    FarSettingsItem item{sizeof(item), root, name, FST_QWORD};
    item.Number = value;
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_SET, 0, &item);
  }
  void SetData(size_t root, const wchar_t* name, const std::vector<uint8_t>& data) {
    FarSettingsItem item{sizeof(item), root, name, FST_DATA};
    item.Data.Size = data.size();
    item.Data.Data = data.data();
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_SET, 0, &item);
  }
  void Delete(size_t root, const wchar_t* name) {
    FarSettingsValue value{sizeof(value), root, name};
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_DELETE, 0, &value);
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

constexpr size_t kRoot = 0;

std::wstring Trim(const std::wstring& text) {
  size_t a = text.find_first_not_of(L" \t");
  if (a == std::wstring::npos) return L"";
  size_t b = text.find_last_not_of(L" \t");
  return text.substr(a, b - a + 1);
}

}  // namespace

Options& Opt() {
  static Options options;
  return options;
}

void LoadOptions() {
  Settings s;
  Options& o = Opt();
  o.addToDiskMenu = s.GetNumber(kRoot, L"AddToDiskMenu", 1) != 0;
  o.useEnv = s.GetNumber(kRoot, L"UseEnvToken", 1) != 0;
  o.useGhCli = s.GetNumber(kRoot, L"UseGhCli", 1) != 0;
  o.prefix = s.GetString(kRoot, L"Prefix", L"gh");
  if (o.prefix.empty()) o.prefix = L"gh";
}

void SaveOptions() {
  Settings s;
  const Options& o = Opt();
  s.SetNumber(kRoot, L"AddToDiskMenu", o.addToDiskMenu);
  s.SetNumber(kRoot, L"UseEnvToken", o.useEnv);
  s.SetNumber(kRoot, L"UseGhCli", o.useGhCli);
  s.SetString(kRoot, L"Prefix", o.prefix);
}

std::string SavedToken() {
  Settings s;
  return UnprotectString(s.GetData(kRoot, L"Token"));
}

bool HasSavedToken() {
  Settings s;
  return !s.GetData(kRoot, L"Token").empty();
}

void SetSavedToken(const std::string& token) {
  Settings s;
  if (token.empty()) {
    s.Delete(kRoot, L"Token");
  } else {
    s.SetData(kRoot, L"Token", ProtectString(token));
  }
}

std::vector<std::string> ExtraOwners() {
  Settings s;
  std::string list = fui::U(s.GetString(kRoot, L"Owners", L""));
  std::vector<std::string> owners;
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find('\n', start);
    std::string owner = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!owner.empty()) owners.push_back(owner);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return owners;
}

void SetExtraOwners(const std::vector<std::string>& owners) {
  std::string list;
  for (const auto& o : owners) {
    if (!list.empty()) list.push_back('\n');
    list += o;
  }
  Settings s;
  s.SetString(kRoot, L"Owners", fui::W(list));
}

bool ConfigureDialog() {
  Options o = Opt();
  std::wstring tokenLabel = HasSavedToken() ? L"Saved &token (one is saved; type \"-\" to remove it):"
                                            : L"Saved &token (optional; personal access token):";
  std::vector<FarDialogItem> items{
      fui::Item(DI_DOUBLEBOX, 3, 1, 66, 15, L"GitHub panel"),
      fui::Item(DI_CHECKBOX, 5, 2, 0, 2, L"Add to &disk menus", DIF_NONE, o.addToDiskMenu),
      fui::Item(DI_TEXT, 5, 3, 0, 3, L"Command line &prefix:"),
      fui::Item(DI_EDIT, 28, 3, 40, 3, o.prefix.c_str()),
      fui::Item(DI_TEXT, -1, 4, 0, 4, L"Login (tried in this order)", DIF_SEPARATOR),
      fui::Item(DI_TEXT, 5, 5, 0, 5, tokenLabel.c_str()),
      fui::Item(DI_PSWEDIT, 5, 6, 64, 6, L""),
      fui::Item(DI_CHECKBOX, 5, 7, 0, 7, L"Use the GH_TOKEN / GITHUB_TOKEN &environment variables", DIF_NONE, o.useEnv),
      fui::Item(DI_CHECKBOX, 5, 8, 0, 8, L"Use the &gh CLI login (gh auth token)", DIF_NONE, o.useGhCli),
      fui::Item(DI_TEXT, 5, 9, 0, 9, L"With none of these, only public data is shown (60 requests/hour)."),
      fui::Item(DI_TEXT, 5, 11, 0, 11, L"The saved token is encrypted for your Windows account (DPAPI)."),
      fui::Item(DI_TEXT, -1, 13, 0, 13, L"", DIF_SEPARATOR),
      fui::Item(DI_BUTTON, 0, 14, 0, 14, L"OK", DIF_CENTERGROUP | DIF_DEFAULTBUTTON),
      fui::Item(DI_BUTTON, 0, 14, 0, 14, L"Cancel", DIF_CENTERGROUP),
  };
  fui::DialogResult r = fui::RunDialog(ConfigDialogGuid, 70, 17, nullptr, items);
  if (r.exitCode != 12) return false;
  bool loginChanged = false;
  o.addToDiskMenu = r.checks[1];
  o.prefix = Trim(r.texts[3]);
  if (o.prefix.empty()) o.prefix = L"gh";
  loginChanged |= o.useEnv != r.checks[7] || o.useGhCli != r.checks[8];
  o.useEnv = r.checks[7];
  o.useGhCli = r.checks[8];
  std::wstring token = Trim(r.texts[6]);
  if (token == L"-") {
    SetSavedToken({});
    loginChanged = true;
  } else if (!token.empty()) {
    SetSavedToken(fui::U(token));
    loginChanged = true;
  }
  Opt() = o;
  SaveOptions();
  return loginChanged;
}

}  // namespace config

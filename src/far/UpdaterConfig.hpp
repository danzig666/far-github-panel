#pragma once

// Per-plugin settings for Updater.cpp.

#include "../core/Json.hpp"
#include "../core/Text.hpp"
#include "Version.hpp"

namespace updcfg {
namespace core = ::gh;
inline constexpr const char* kOwner = "danzig666";
inline constexpr const char* kRepo = "far-github-panel";
inline constexpr const char* kDllName = "GitHubPanel.dll";  // release asset, plus "GitHubPanel.dll.sha256"
inline constexpr const char* kVersion = GHP_VERSION_STRING;
inline constexpr const wchar_t* kProduct = L"GitHub panel";
}  // namespace updcfg

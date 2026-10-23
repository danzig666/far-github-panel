GitHub panel for Far Manager 3
==============================

A read-only Far Manager panel for browsing GitHub: your account and
organizations, their repositories, branches, tags, releases and files.

Requirements
------------
- Far Manager 3.0.6638 or newer, 64-bit (x64)
- Internet access to api.github.com (the system proxy settings are used)

Installation
------------
1. Close Far Manager.
2. Copy the GitHubPanel folder from this zip into your personal plugins
   folder, %FARPROFILE%\Plugins\ (usually %APPDATA%\Far Manager\Profile\Plugins),
   or into %FARHOME%\Plugins\. The personal folder needs no admin rights and
   lets the plugin update itself (see "Updating").
   You should end up with ...\Plugins\GitHubPanel\GitHubPanel.dll
3. Start Far Manager.

Login
-----
Nothing to set up if you use the GitHub CLI: the plugin runs
"gh auth token" and uses your gh login. The plugin tries, in order:
  1. a token saved in the plugin's settings (encrypted for your Windows user)
  2. the GH_TOKEN, then GITHUB_TOKEN environment variables
  3. the gh CLI login (gh.exe must be on PATH; run "gh auth login" once)
  4. anonymous: public data only, 60 requests per hour
The top level's title shows who you are and where the login came from.

Opening
-------
- Alt+F1 / Alt+F2 (disk menu) > GitHub
- Command line:  gh:                              your owners
                 gh:octocat                       an owner's repositories
                 gh:owner/repo                    a repository
                 gh:owner/repo/src/core           a folder
                 gh:https://github.com/owner/repo/tree/branch/path

What you see
------------
/                          you, your organizations, owners you added
/owner                     repositories (Ctrl+6 shows descriptions)
/owner/repo                files of the default branch, plus:
   [branches]              every branch; open one to browse its files
   [tags]                  every tag; open one to browse its files
   [releases]              releases; open one for its files and source zips
Branch and tag names containing "/" are shown with a similar-looking slash.
File dates are the date of the branch's latest commit (GitHub does not give
per-file dates without one request per file).

Keys
----
  Enter            open
  F3 / Ctrl+Q      view a file
  F4               view a file (this panel is read-only)
  F5               copy to your PC: files, folders, release files. On a
                   repository, branch or tag: download it as a .zip
  F7 / F8          on the top level only: add / remove an owner in the list
                   (nothing changes on GitHub)
  Shift+F1         actions: open in browser, copy URL, clone with git
                   (puts the command on the command line), download ZIP,
                   rate limit and login, log in again, check for updates
  Ctrl+R           refresh
  Ctrl+L           info panel: repository details, commit, requests left

During a download, Esc or F8 asks "Stop the download?" with Continue as the
default button. Files are written as "name.ghpart" and renamed when done.

Rate limits
-----------
Logged in: 5,000 requests per hour. Listings you have seen before are
re-checked with conditional requests, which GitHub does not count when
nothing changed, so browsing back and forth is cheap. Shift+F1 > Rate limit
shows what is left.

Settings
--------
F9 > Options > Plugins configuration > GitHub panel: disk menu entry,
command line prefix, saved token, and whether to use the environment
variables and the gh CLI.

Updating
--------
Shift+F1 > Check for updates asks GitHub for the latest release. If there is
a newer one, "Update now" downloads it, checks its SHA-256 checksum, and swaps
it in while Far keeps running (Windows lets a loaded DLL be renamed, not
overwritten). Restart Far to use the new version; Far windows that are open
keep the old one until they restart.

The old file is kept as "GitHubPanel.dll.<version>-<time>.old" and deleted
automatically - at Far start or after the next update - but only once no
running Far has it loaded. If a Far running as administrator cannot be
checked, nothing is deleted that time.

This needs the plugin to be in a folder you can write to. Far also loads
plugins from your profile, which needs no admin rights:
  %FARPROFILE%\Plugins   (usually %APPDATA%\Far Manager\Profile\Plugins)
Install the plugin there instead of under Program Files (and remove the copy
in Program Files, or Far finds the same plugin twice).

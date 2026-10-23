### 0.2.1

- The panel remembers where you were. Opening it again (disk menu, F11, or a bare `gh:`) returns to the last folder, e.g. `/danzig666/repo-example/src`, including branch and tag folders. If that place no longer exists it opens the top level instead. `gh:/` always opens the top level.

### 0.2.0

- **Shift+F1 → Check for updates**. If a newer release exists, *Update now* downloads it, verifies its SHA-256 checksum and swaps it in while Far keeps running. Restart Far to use it.
- The replaced DLL is kept as `GitHubPanel.dll.<version>-<time>.old` and deleted automatically (at Far start or after the next update) only once no running `far.exe` has it loaded.
- Self-update needs the plugin in a folder you can write to. Far also loads plugins from `%FARPROFILE%\Plugins` (usually `%APPDATA%\Far Manager\Profile\Plugins`), which needs no admin rights: install it there instead of under Program Files.
- Releases now also include the bare `GitHubPanel.dll` and `GitHubPanel.dll.sha256` used by the updater.

### 0.1.0

First release: a read-only GitHub panel for Far Manager 3.

**Requirements:** Far Manager 3.0.6638+ (x64).

**Install:** unzip and copy the `GitHubPanel` folder into `%FARPROFILE%\Plugins\` (recommended, no admin rights needed) or `%FARHOME%\Plugins\`, then restart Far. Keep only one copy. Open it from the disk menu (Alt+F1/Alt+F2 → GitHub) or type `gh:` / `gh:owner/repo` on the command line.

**Login:** uses your existing `gh` CLI login automatically. Also supports `GH_TOKEN` / `GITHUB_TOKEN` or a token saved (DPAPI-encrypted) in the plugin settings; without any, public data only.

### Features
- Your account, organizations and any owner you add; their repositories with private/fork/stars/language
- Files of the default branch, plus `[branches]`, `[tags]` and `[releases]` folders
- F3/F4 view, F5 copy files and whole folders, release assets, and repos/branches/tags as `.zip`
- Git LFS files are downloaded with their real content
- Shift+F1: open in browser, copy URL, clone with git, download ZIP, rate limit and login
- Opens `github.com` URLs from the command line (`gh:https://github.com/...`)
- Conditional requests (ETags) so refreshing does not use up the rate limit

### Known limitations
- Read-only: no commits, uploads or deletes.
- github.com only (no GitHub Enterprise Server yet).
- File dates are the latest commit's date for the whole tree.

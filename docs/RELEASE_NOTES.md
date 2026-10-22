First release: a read-only GitHub panel for Far Manager 3.

**Requirements:** Far Manager 3.0.6638+ (x64).

**Install:** unzip, copy the `GitHubPanel` folder into `%FARHOME%\Plugins\`, restart Far. Open it from the disk menu (Alt+F1/Alt+F2 → GitHub) or type `gh:` / `gh:owner/repo` on the command line.

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

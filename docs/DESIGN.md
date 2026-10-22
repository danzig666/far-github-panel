# far-github-panel: design

A read-only Far Manager 3 panel plugin for browsing GitHub: your account and
organizations, their repositories, branches, tags, releases and file trees.
It works like the Network plugin or NetBox. Open it from the disk menu, press
Enter to go into a folder, F3 to view a file, and F5 to copy to the PC.

| | |
|---|---|
| Host | Far Manager 3, build **3.0.6638 or newer**, **Windows x64** |
| Language | C++20, Far 3 SDK (`plugin.hpp`), WinHTTP (HTTPS), no other runtime dependencies |
| Build | CMake + MSVC on GitHub Actions `windows-latest`, static CRT, one `GitHubPanel.dll` |
| API | GitHub REST API v3 (`api.github.com`, `X-GitHub-Api-Version: 2022-11-28`) |
| Login | reuses the `gh` CLI login; also `GH_TOKEN` / `GITHUB_TOKEN`, a saved token, or anonymous |

The structure follows [far-ps5-filemanager](https://github.com/danzig666/far-ps5-filemanager):
a platform-neutral core, tested on Linux and in CI, and a thin Windows layer
for Far.

---

## 1. Scope

**v1 (read-only)**

1. Browse owners (you, your organizations, any user or org you type in),
   their repositories, and the repository tree on any branch or tag.
2. Releases as folders. Their assets and the source zip/tarball are files.
3. F3 view, Ctrl+Q quick view and F5 copy to the PC for files and whole
   folders. F5 on a repository, branch or tag downloads it as a `.zip`.
4. Shift+F1 actions: open in the browser, copy the URL, put a `git clone`
   command on the command line, show the rate limit and login, refresh.
5. Login through `gh` with no setup needed.

**Not in v1:** any write to GitHub (commits, uploads, deletes, issues, PRs).
F6, F7, F8, Shift+F6 and copying into the panel show "This panel is
read-only". Also out: GitHub Enterprise Server hosts (only `github.com`;
the base URL is one constant, so adding a host setting later is small),
issues/PRs/Actions folders, and per-file commit dates (section 4.3).

---

## 2. Panel hierarchy

Every location is a path of components, which keeps navigation, `CurDir`,
shortcuts and the command line consistent:

```
/                                      owners: you, your orgs, others you opened
/danzig666                             repositories of that owner
/danzig666/far-ps5-filemanager         default branch, repository root
/danzig666/far-ps5-filemanager/src/core            ...any folder in it
/danzig666/far-ps5-filemanager/[branches]          all branches
/danzig666/far-ps5-filemanager/[branches]/dev/src  tree of branch "dev"
/danzig666/far-ps5-filemanager/[tags]              all tags
/danzig666/far-ps5-filemanager/[tags]/v1.0.0       tree of tag v1.0.0
/danzig666/far-ps5-filemanager/[releases]          all releases
/danzig666/far-ps5-filemanager/[releases]/v1.0.0   assets + source archives
```

- The default branch is shown directly under the repository, so the common
  case is one Enter away. `[branches]`, `[tags]` and `[releases]` are listed
  first in the repository root. A real top-level folder with one of those
  exact names is hidden by them; that is rare and accepted.
- Branch and tag names may contain `/` (`feature/x`). In item names a `/`
  is shown as `∕` (U+2215) so Far does not treat it as a path separator.
  The plugin maps display names back to real names through the listing it
  returned.
- `..` from a repository goes to the owner, and from an owner to `/`. `/`
  is the top level, so `..` there closes the panel (Far's rule for an empty
  `CurDir`).

### Items and columns

| Level | Name | Size | Date | Description (Ctrl+6) |
|---|---|---|---|---|
| `/` | login / org | — | — | "you", "organization", "user" |
| owner | repo | repo size | last push | `private`/`fork`/`archived`, ★ stars, language, description |
| `[branches]` / `[tags]` | ref name | — | — | "default", short commit SHA |
| tree | file / folder | blob size | commit date of the ref | mode (`exec`, `symlink`, `submodule`) |
| `[releases]` | tag | total asset size | published | name, `draft`/`pre-release` |
| release | asset / `source.zip` / `source.tar.gz` | asset size | updated | download count |

Owners, repos, refs, releases and tree folders are directories, and
everything else is a file. Hidden attribute: names starting with `.`.
Submodules show as empty folders that cannot be opened; the description
says where they point.

---

## 3. Login

`TokenProvider` tries these sources in order and remembers which one won (the
root panel's title shows it, e.g. ` GitHub: danzig666 (gh) `):

1. **Saved token**: a token entered in the plugin's configuration. Stored in
   Far's settings, encrypted with DPAPI (`CryptProtectData`, current user).
2. **Environment**: `GH_TOKEN`, then `GITHUB_TOKEN` (the same order as `gh`).
3. **`gh` CLI**: runs `gh auth token --hostname github.com` hidden
   (`CreateProcessW`, `CREATE_NO_WINDOW`, 10 s timeout) and reads stdout. This
   works whether `gh` keeps the token in Windows Credential Manager or in its
   `hosts.yml`, and needs no knowledge of `gh`'s storage format. `gh` must be
   on `PATH`. If it isn't, or it reports "not logged in", this source is skipped.
4. **Anonymous**: public data only, 60 requests per hour. The root panel then
   says so and points to `gh auth login`.

A `401 Bad credentials` with a token drops the cached token and retries the
next source once (for example, an expired saved token falls back to `gh`).
"Refresh login" in the Shift+F1 menu re-runs the chain.

Scopes: classic `gh` tokens normally have `repo` and `read:org`, which covers
private repositories and organization membership. Fine-grained tokens see
only what they were granted, and the plugin just shows what the API returns.

---

## 4. GitHub API mapping

All requests send `Accept: application/vnd.github+json`,
`X-GitHub-Api-Version: 2022-11-28`, `User-Agent: far-github-panel/<version>`
and, if logged in, `Authorization: Bearer <token>`.

| Purpose | Request | Notes |
|---|---|---|
| Who am I | `GET /user` | login, for the root panel |
| My orgs | `GET /user/orgs` | paged |
| My repos | `GET /user/repos?affiliation=owner&sort=pushed` | includes private |
| Org repos | `GET /orgs/{org}/repos?sort=pushed` | private ones if the token allows |
| Other user's repos | `GET /users/{user}/repos?sort=pushed` | public only |
| Owner type | `GET /users/{name}` | `type`: `User` / `Organization` |
| Repo info | `GET /repos/{o}/{r}` | default branch, description, size |
| Branches | `GET /repos/{o}/{r}/branches` | paged |
| Tags | `GET /repos/{o}/{r}/tags` | paged |
| Resolve ref | `GET /repos/{o}/{r}/commits/{ref}` | commit SHA, tree SHA, committer date |
| Tree | `GET /repos/{o}/{r}/git/trees/{tree_sha}?recursive=1` | one request for the whole repo |
| Subtree | `GET /repos/{o}/{r}/git/trees/{sha}` | used when the recursive tree is `truncated` |
| File bytes | `GET /repos/{o}/{r}/git/blobs/{sha}` with `Accept: application/vnd.github.raw` | up to 100 MB |
| LFS file | `GET https://media.githubusercontent.com/media/{o}/{r}/{ref}/{path}` | when the blob is an LFS pointer |
| Releases | `GET /repos/{o}/{r}/releases` | assets included in each release |
| Asset bytes | `GET /repos/{o}/{r}/releases/assets/{id}` with `Accept: application/octet-stream` | 302 to a signed URL |
| Source archive | `GET /repos/{o}/{r}/zipball/{ref}` / `tarball/{ref}` | 302 to `codeload.github.com` |
| Rate limit | `GET /rate_limit` | free; also read from response headers |

### 4.1 Pagination

`per_page=100`, following `Link: <...>; rel="next"` until it is absent. To
stay responsive, listings stop at 3,000 items, and the panel then ends with
an item `... more (open on github.com)`.

### 4.2 Caching and rate limits

- **ETags:** every JSON GET is cached in memory by URL with its `ETag`. The
  next request sends `If-None-Match`. A `304 Not Modified` reuses the cached
  body and does **not** count against the rate limit, so Ctrl+R and
  re-entering folders are almost free.
- **Trees** are cached per tree SHA. A tree SHA never changes, so this cache
  never needs invalidating; a moved branch simply resolves to a new SHA.
- **Ref resolution** is cached for 60 seconds per (repo, ref). Ctrl+R clears
  that cache for the current repository.
- `X-RateLimit-Remaining` / `X-RateLimit-Reset` are recorded from every
  response. When the limit is hit (`403` or `429` with remaining 0), the
  error says when it resets, and for anonymous use it suggests logging in
  with `gh`. A secondary rate limit (`retry-after`) is shown the same way.

### 4.3 Dates in the tree

The trees API has no per-file dates. Getting the last commit for each file
would cost one request per file. So every item in a tree gets the date of
the commit the ref points to. The panel title shows the short SHA and date,
e.g. ` far-ps5-filemanager [main @ 37e9206, 2026-10-22] `.

### 4.4 Large trees

If the recursive tree comes back `truncated` (over 100,000 entries or
7 MB), the plugin keeps the part it got and loads any folder that is missing
with a non-recursive subtree request when it is opened.

### 4.5 Downloads and redirects

Asset and archive downloads answer `302` with a signed URL on another host.
The transport does **not** follow redirects itself. The client follows up to
5 redirects and **drops the `Authorization` header when the host changes**,
so the token is never sent to S3 or codeload, which would also reject the
extra auth header. Blob downloads have no redirect.

LFS: a blob of at most 1 KB that starts with
`version https://git-lfs.github.com/spec/v1` is an LFS pointer. On download
the file is fetched from `media.githubusercontent.com` with the same token.
The size column shows the pointer's size, because finding the real size
would cost one request per small file.

---

## 5. Far integration

### 5.1 Keys

| Key | Action |
|---|---|
| Enter | open owner / repo / folder / ref / release |
| F3, Ctrl+Q | view a file (downloaded to Far's temp folder) |
| F4 | opens the file in Far's viewer (read-only panel) |
| F5 | copy to the PC: files, folders (recursively from the cached tree), assets. On a repo, branch or tag: download `<repo>-<ref>.zip` |
| F6, F7, F8, Shift+F6 | "This panel is read-only" |
| Shift+F1 | actions menu (5.3) |
| Ctrl+R | refresh (clears ETags-protected caches for the current level) |
| Alt+F7 (Find) | works inside a repository tree. It is served from the cached tree, so it costs no requests |

F5 shows a "Copy N items to:" dialog prefilled with the passive panel's
folder, the same overwrite/skip/all prompts as far-ps5-filemanager, and a
progress window with bytes, speed and time left. Esc or F8 asks "Stop the
download?", and **Continue** is the default. Downloads go to `name.ghpart`
and are renamed when complete.

### 5.2 Opening

- Disk menu (Alt+F1/F2) → **GitHub**: opens `/`.
- Command line `gh:` prefix:
  - `gh:` opens `/`
  - `gh:danzig666` opens an owner
  - `gh:danzig666/far-ps5-filemanager` opens a repo
  - `gh:danzig666/far-ps5-filemanager/src/core` opens a path
  - `gh:https://github.com/owner/repo/tree/branch/path` opens a URL pasted from the browser. Because branches may contain `/`, the plugin tries the longest matching branch name, the same way github.com does.
- Folder shortcuts (Ctrl+0..9) store the path (`OPIF_SHORTCUT`).
- F7 at `/` asks for an owner name and adds it to the list (remembered in
  settings). F8 at `/` removes such an added owner. These are local list
  edits, not GitHub writes.

### 5.3 Actions menu (Shift+F1)

- Open in browser: repo, folder, file (`/blob/`), release, or owner.
- Copy URL: `FSF.CopyToClipboard`.
- Clone: puts `git clone https://github.com/o/r.git "<passive panel folder>\r"`
  on Far's command line without running it, so the user sees it and presses
  Enter.
- Download as ZIP: zipball of the current ref.
- Rate limit and login: the token source, login, scopes
  (`X-OAuth-Scopes`), and remaining/limit/reset for `core`.
- Refresh login: re-runs the token chain (section 3).

### 5.4 Info panel (Ctrl+L)

`OpenPanelInfo.InfoLines`: owner and repo, description, default branch,
current ref and commit, visibility, stars, forks, license, rate limit
remaining.

---

## 6. Architecture

```
src/core/                 platform-neutral, unit-tested on Linux
  Json.*  Text.*          from far-ps5-filemanager
  Http.hpp                URL-based transport interface (https, no redirects)
  Url.*                   URL parsing, Link header parsing
  GitHubClient.*          typed API, ETag cache, pagination, redirects, rate limit
  RepoTree.*              recursive tree -> folder map, truncated subtrees, LFS pointers
  Location.*              path components <-> levels, github.com URL parsing
src/far/                  Windows only
  Plugin.cpp              exports
  Panel.*                 navigation, listings, F5/F3, actions
  Token.*                 token chain: saved (DPAPI), env, gh CLI
  WinHttpTransport.*      WinHTTP with WINHTTP_FLAG_SECURE, redirects disabled
  FarApi.* Progress.* Config.*   from far-ps5-filemanager
tests/
  unit_tests.cpp          JSON, Link/URL parsing, tree model, LFS detection,
                          client requests against a scripted transport
  CurlHttp.*              libcurl transport (Linux CI)
  integration_tests.cpp   real api.github.com: Linux+libcurl in CI
  winhttp_smoke.cpp       real api.github.com through WinHttpTransport (Windows CI)
```

`HttpTransport::Send(HttpRequest&)` takes a full URL (`https://host/path?query`).
The client owns everything GitHub-specific, and the transports only move bytes.

Threading follows far-ps5-filemanager: everything runs synchronously on Far's
thread, with progress windows for long downloads and explicit WinHTTP
timeouts (connect 10 s, send/receive 30 s per chunk).

---

## 7. Errors

| Situation | Message |
|---|---|
| 401 | "GitHub rejected the token from <source>." Then the next source is tried once (section 3). |
| 403/429 with rate limit at 0 | "GitHub rate limit reached; resets at 14:05." Anonymous adds "Log in with `gh auth login` for 5,000 requests per hour." |
| 403 otherwise | GitHub's `message` (e.g. SSO enforcement, with the URL to authorize) |
| 404 on a repo or owner | "Not found, or your login cannot see it." |
| 409 on an empty repo | the repo shows an empty panel with the description "empty repository" |
| network | "Cannot reach api.github.com: <WinHTTP error>" |

---

## 8. Build, test, release

- CMake, the same layout as far-ps5-filemanager. Links `winhttp` and
  `crypt32` (DPAPI) only.
- CI (`.github/workflows/build.yml`):
  - Linux: unit tests (`-Werror`).
  - Linux: integration tests with libcurl against the real API, using the
    workflow's `GITHUB_TOKEN`. Public targets only: `octocat` repos,
    `octocat/Hello-World` tree and blob, a `cli/cli` release asset
    (`*checksums.txt`, small) through the redirect path, and the
    "no Authorization after redirect" check.
  - Windows: the plugin DLL, unit tests, a WinHTTP smoke test against the
    real API (TLS + redirect), and the exports and VC++-runtime checks.
  - Release on a `v*` tag or a manual run with `release_tag` set.
- Manual checklist: login via `gh` with no setup, private repo browsing,
  an org with SSO, an LFS file, a repo over 100k files, a branch with `/`,
  a release with many assets, and the anonymous rate-limit message.

---

## 9. Milestones

| Phase | Scope |
|---|---|
| 1 | Core client, tree model, token chain, browse `/` → owner → repo → tree, F3/F5 files and folders, CI |
| 2 | Branches/tags/releases, assets and archives with redirects, LFS, Shift+F1 actions, info panel, URL opening |
| later, if wanted | write support (commits via the contents / Git Data API), issues/PRs folders, GitHub Enterprise hosts |

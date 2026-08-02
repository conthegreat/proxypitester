# Hosting updates for ProxyPiTester (no app secrets / no API tokens required)

The Windows app checks for updates over **HTTPS** against **public** GitHub content.
You do **not** need to put GitHub credentials inside the EXE, and you do **not** need
to give anyone a personal access token for basic public updates.

## Recommended setup

1. Create a **public** GitHub repo, e.g. `https://github.com/conthegreat/proxypitester`
2. Put `update.json` in the **main** branch (repo root), like this:

```json
{
  "version": "1.0.1",
  "download_url": "https://github.com/conthegreat/proxypitester/releases/latest",
  "notes": "Bug fixes and UI polish."
}
```

3. Publish a **Release** (tag `v1.0.1`) and attach `ProxyPiTester-v1.0.1-win64.zip`.
4. When you ship a new build:
   - Bump `APP_VERSION` in `src_simple/main.cpp` (e.g. `1.0.1`)
   - Rebuild Release
   - Upload the new zip to a new GitHub Release
   - Edit `update.json` so `"version"` matches (e.g. `1.0.1`)

## What the app does

| Source | URL used by the app |
|--------|---------------------|
| Primary | `https://raw.githubusercontent.com/conthegreat/proxypitester/main/update.json` |
| Fallback | `https://api.github.com/repos/conthegreat/proxypitester/releases/latest` |

- **Help → Check for Updates…** — always reports result.
- On startup — quiet check; only prompts if a newer version is found.
- If update exists — offers to open the download page in the browser (user installs manually).

## Private repos

If the repo is **private**, raw/API checks need authentication. That means embedding a
token in the client (bad idea — anyone can extract it) or hosting `update.json` on a
**public** URL (GitHub Pages, your website, S3, etc.) instead.

**Best practice:** keep the update feed **public**; never put proxy passwords or tokens in the app.

## Version format

Use `major.minor.patch` such as `1.0.0`, `1.0.1`, `1.2.0`. Optional leading `v` is OK (`v1.0.1`).

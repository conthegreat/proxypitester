# ProxyPiTester

Windows desktop tool for [ProxyPi](https://proxypi.co.uk) — **UK residential SOCKS5 / HTTP** proxy connectivity and speed tests.

![Windows](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![Version](https://img.shields.io/badge/version-1.0.0-00bcd4)

## Download

Grab the latest release: **[Releases](https://github.com/conthegreat/proxypitester/releases/latest)**

Zip contains `ProxyPiTester.exe` only (no credentials).

## Features

- SOCKS5 / HTTP proxy test (exit IP + latency)
- Download speed test through your proxy (~1 MB)
- Save / reload settings locally (`ProxyPiTester.ini` on your PC only)
- Help menu: About, Website, **Check for Updates**
- Quiet startup update check against public `update.json` / GitHub Releases

## Build (Visual Studio 2022)

1. Open `ProxyTools.sln` (or `ProxyPiTester.vcxproj`)
2. Set configuration **Release | x64**
3. Build project **ProxyPiTester**
4. Output: `bin\Release\ProxyPiTester.exe`

## Update feed

Public file: [`update.json`](./update.json)

```json
{
  "version": "1.0.0",
  "download_url": "https://github.com/conthegreat/proxypitester/releases/latest",
  "notes": "..."
}
```

When shipping a new build: bump `APP_VERSION` in `src_simple/main.cpp`, update this file, tag a Release.

See [docs/GITHUB_UPDATES.md](./docs/GITHUB_UPDATES.md).

## Security

- No proxy host/user/password is embedded in the EXE
- Do not commit `proxydetails.txt`, SSH notes, or `*.ini` with real credentials
- Update checks use **public** HTTPS only (no tokens in the client)

## License

Personal / ProxyPi use. Use at your own risk.

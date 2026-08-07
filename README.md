# ProxyPiTester

Windows desktop tool for [ProxyPi](https://proxypi.co.uk) — **UK residential SOCKS5 / HTTP** proxy testing and **app routing**.

![Windows](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![Version](https://img.shields.io/badge/version-1.2.1-00bcd4)

## Download

Grab the latest release: **[Releases](https://github.com/conthegreat/proxypitester/releases/latest)**

Zip contains `ProxyPiTester.exe` only (no credentials).

## Features

- **Login & Load My Proxy** via your ProxyPi account (fills host, ports, credentials)
- SOCKS5 / HTTP connectivity test (exit IP + latency)
- Download speed test through your proxy
- **App routing** through a shared local SOCKS bridge (auth handled for you):
  - Chrome, Edge, Brave, Firefox, Opera, Vivaldi
  - Discord, Slack, Teams, VS Code, Cursor, Postman, Thunderbird, Spotify
  - RuneLite, Jagex Launcher
  - Browse any Chromium / Electron `.exe`
- Live **Active Apps** panel (PID, connections, bridge port)
- Open a URL via a proxied browser
- Copy exit IP + session notes (for support)
- Save / reload settings locally (`ProxyPiTester.ini` on your PC only)
- Help: About, Website, Check for Updates

## Build (Visual Studio 2022)

1. Open `ProxyPiTester.vcxproj` (or `ProxyTools.sln` if present)
2. Set configuration **Release | x64**
3. Build project **ProxyPiTester**
4. Output: `bin\Release\ProxyPiTester.exe`

## Update feed

Public file: [`update.json`](./update.json)

When shipping a new build: bump `APP_VERSION` in `src_simple/main.cpp`, update this file and `update.json`, tag a Release.

See [docs/GITHUB_UPDATES.md](./docs/GITHUB_UPDATES.md).

## Security

- No proxy host/user/password is embedded in the EXE
- Do not commit credentials, SSH notes, or `*.ini` with real secrets
- Update checks use **public** HTTPS only (no tokens in the client)

## Support

https://proxypi.co.uk · support@proxypi.co.uk

## License

Personal / ProxyPi use. Use at your own risk.

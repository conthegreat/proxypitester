#pragma once

#include "common.h"
#include <string>
#include <vector>

// Local no-auth SOCKS5 (or HTTP CONNECT) listener that forwards through the real
// upstream proxy WITH authentication.
//
// Why: Chromium's --proxy-server=socks5://user:pass@host often fails against
// servers that require SOCKS method 2; Chrome may not complete auth correctly.
// Pointing Chrome at 127.0.0.1:<localPort> with NO credentials works, while we
// speak method 2 to the real proxy.
//
// Multi-app: one bridge port is shared; many clients may connect at once.

// Start (or reconfigure) the bridge. Returns local listen port on 127.0.0.1.
// Thread-safe; safe to call again with same/different config.
bool EnsureLocalAuthBridge(const ProxyConfig& cfg, int& outLocalPort, std::wstring& err);

// Stop the background acceptor (optional; process exit also tears it down).
void StopLocalAuthBridge();

// True if a bridge is currently listening.
bool IsLocalAuthBridgeRunning();

// Current bridge listen port, or 0 if not running.
int GetLocalAuthBridgePort();

// ---- Routed app sessions (multi-app monitor) ----
struct RoutedAppSession {
  DWORD pid = 0;           // primary PID (launch PID or first bridge client)
  std::wstring name;       // Chrome, Edge, Firefox, RuneLite, Jagex
  std::wstring method;     // chromium / cef / java-socks / firefox-profile / wrap
  int bridgePort = 0;
  DWORD startTick = 0;
  bool alive = true;
  int connCount = 0;       // established sockets to local bridge (0 if only registered)
  int processCount = 1;    // processes in this app family currently seen
};

void RegisterRoutedApp(DWORD pid, const std::wstring& name, const std::wstring& method, int bridgePort);
// Snapshot for UI: merges launched apps + live TCP clients on the bridge.
// Browsers multi-process (Chrome/Firefox) are grouped by name so the list stays readable.
std::vector<RoutedAppSession> GetRoutedAppSessions();
void ClearRoutedAppSessions();

// ---- Path discovery ----
std::wstring FindDefaultChromePath();
std::wstring FindDefaultEdgePath();
std::wstring FindDefaultBravePath();
std::wstring FindDefaultOperaPath();
std::wstring FindDefaultVivaldiPath();
std::wstring FindDefaultFirefoxPath();
std::wstring FindDefaultDiscordPath();
std::wstring FindDefaultSlackPath();
std::wstring FindDefaultTeamsPath();
std::wstring FindDefaultVSCodePath();
std::wstring FindDefaultCursorPath();
std::wstring FindDefaultPostmanPath();
std::wstring FindDefaultThunderbirdPath();
std::wstring FindDefaultSpotifyPath();
std::wstring FindDefaultRuneLiteDir();
std::wstring FindDefaultJagexLauncherPath();

// True if basename looks like Chromium/Electron (chrome, edge, brave, discord, opera, …).
bool IsChromiumLikeExecutable(const std::wstring& exePath);

// True if any process with this image name is running (e.g. L"Discord.exe").
bool IsImageRunning(const std::wstring& imageBase);

// ---- Launches (all share the same local auth bridge) ----

// Generic Chromium / Electron via --proxy-server=socks5://127.0.0.1:<port>.
// useTempProfile=true isolates browsers; false keeps app data (needed for Discord login).
// startUrl empty = no URL arg; browsers default callers pass ifconfig.me.
bool LaunchChromiumLikeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                                 const std::wstring& exePath,
                                 const std::wstring& displayName,
                                 bool useTempProfile,
                                 const std::wstring& startUrl = L"");

// Chromium browsers: disposable profile + proxy
bool LaunchChromeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& chromePath = L"",
                           const std::wstring& startUrl = L"");
bool LaunchEdgeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                         const std::wstring& edgePath = L"",
                         const std::wstring& startUrl = L"");
bool LaunchBraveViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& bravePath = L"",
                          const std::wstring& startUrl = L"");

// Discord (Electron): real profile, proxy only — keeps your Discord login.
bool LaunchDiscordViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& discordPath = L"");

// Other Electron / Chromium-family apps (keep real profile unless noted).
bool LaunchOperaViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& path = L"", const std::wstring& startUrl = L"");
bool LaunchVivaldiViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& path = L"", const std::wstring& startUrl = L"");
bool LaunchSlackViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& path = L"");
bool LaunchTeamsViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& path = L"");
bool LaunchVSCodeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& path = L"");
bool LaunchCursorViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& path = L"");
bool LaunchPostmanViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& path = L"");
// Thunderbird: temp profile with SOCKS prefs (like Firefox).
bool LaunchThunderbirdViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                                const std::wstring& path = L"");
// Spotify: Electron-style proxy flag (best-effort; Store builds vary).
bool LaunchSpotifyViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& path = L"");

// Open a URL through a proxied browser (Chrome preferred, then Edge, Brave).
bool LaunchUrlViaProxiedBrowser(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                                const std::wstring& url);

// Firefox: temp profile user.js -> SOCKS 127.0.0.1 (no auth); bridge auths upstream.
bool LaunchFirefoxViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& firefoxPath = L"");

// RuneLite: -J -DsocksProxyHost / Port into client JVM (or java -jar fallback).
bool LaunchRuneLiteViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                             const std::wstring& optionalInstallDir = L"");

// Jagex Launcher: CEF --proxy-server (account/API). Prefer LaunchJagexWithRuneLiteWrap.
bool LaunchJagexViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& optionalExePath = L"");

// --- RuneLite SOCKS wrap (for Jagex "Play" -> RuneLite) ---
bool ApplyRuneLiteSocksWrap(int localSocksPort, std::wstring& err);
bool ClearRuneLiteSocksWrap(std::wstring& err);
bool IsRuneLiteSocksWrapActive();
int  GetRuneLiteSocksWrapPort();

// Bridge + arm RuneLite wrap + launch Jagex.
bool LaunchJagexWithRuneLiteWrap(const ProxyConfig& cfg, DWORD& outPid, int& outBridgePort,
                                 std::wstring& err, const std::wstring& optionalExePath = L"");

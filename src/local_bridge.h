#pragma once

#include "common.h"
#include <string>

// Local no-auth SOCKS5 (or HTTP CONNECT) listener that forwards through the real
// upstream proxy WITH authentication.
//
// Why: Chromium's --proxy-server=socks5://user:pass@host often fails against
// servers that require SOCKS method 2; Chrome may not complete auth correctly.
// Pointing Chrome at 127.0.0.1:<localPort> with NO credentials works, while we
// speak method 2 to the real proxy.

// Start (or reconfigure) the bridge. Returns local listen port on 127.0.0.1.
// Thread-safe; safe to call again with same/different config.
bool EnsureLocalAuthBridge(const ProxyConfig& cfg, int& outLocalPort, std::wstring& err);

// Stop the background acceptor (optional; process exit also tears it down).
void StopLocalAuthBridge();

// True if a bridge is currently listening.
bool IsLocalAuthBridgeRunning();

// Launch Google Chrome (or optional chromePath) through the local auth bridge.
// Uses a disposable --user-data-dir and --proxy-server=socks5://127.0.0.1:<port>.
// No DLL injection. outPid is the chrome.exe PID on success.
bool LaunchChromeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& chromePath = L"");

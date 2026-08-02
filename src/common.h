#pragma once

// Critical: Prevent the old winsock.h from being included by windows.h
// Must define these BEFORE any windows.h or winsock include.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCKAPI_

// Include modern Winsock2 first
#include <winsock2.h>
#include <ws2tcpip.h>

// Now safe to include the full Windows headers
#include <windows.h>

#include <string>
#include <vector>

enum class ProxyType {
  SOCKS5 = 0,
  HTTP = 1
};

struct ProxyConfig {
  ProxyType type = ProxyType::SOCKS5;
  std::wstring host;
  int port = 1080;
  bool useAuth = false;
  std::wstring username;
  std::wstring password;

  // Convert to narrow for low-level use
  std::string hostA() const;
  std::string usernameA() const;
  std::string passwordA() const;
};

// Shared memory config for the hook DLL
#pragma pack(push, 1)
struct HookConfig {
  int proxyType;           // 0 = SOCKS5, 1 = HTTP
  char host[256];
  int port;
  int useAuth;             // 0/1
  char username[128];
  char password[128];
};
#pragma pack(pop)

#define HOOK_SHARED_NAME "Local\\ProxyToolsHookConfig"

// Helper to read/write the hook config via shared memory
bool WriteHookConfig(const ProxyConfig& cfg);
bool ReadHookConfig(HookConfig& out);
void ClearHookConfig();

// Simple ini helpers (next to exe)
std::wstring GetConfigPath();
bool SaveConfigToIni(const ProxyConfig& cfg);
bool LoadConfigFromIni(ProxyConfig& cfg);

// Logging helper (thread safe simple)
void Log(const std::wstring& msg);
void SetLogWindow(HWND hwnd);
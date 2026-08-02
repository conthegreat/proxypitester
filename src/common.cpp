#include "common.h"
#include <shlwapi.h>
#include <vector>
#include <mutex>

#pragma comment(lib, "shlwapi.lib")

static std::mutex g_logMutex;
static HWND g_logEdit = nullptr; // set later by GUI

void SetLogWindow(HWND hwnd) { g_logEdit = hwnd; }

void Log(const std::wstring& msg) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t ts[32];
  swprintf_s(ts, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

  std::wstring line = ts + msg + L"\r\n";

  if (g_logEdit) {
    int len = GetWindowTextLengthW(g_logEdit);
    SendMessageW(g_logEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);
  }
  OutputDebugStringW(line.c_str());
}

std::wstring GetExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  PathRemoveFileSpecW(path);
  return std::wstring(path);
}

std::wstring GetConfigPath() {
  return GetExeDir() + L"\\ProxyTools.ini";
}

bool SaveConfigToIni(const ProxyConfig& cfg) {
  std::wstring path = GetConfigPath();
  WritePrivateProfileStringW(L"Proxy", L"Type", cfg.type == ProxyType::SOCKS5 ? L"SOCKS5" : L"HTTP", path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Host", cfg.host.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Port", std::to_wstring(cfg.port).c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"UseAuth", cfg.useAuth ? L"1" : L"0", path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Username", cfg.username.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Password", cfg.password.c_str(), path.c_str());
  return true;
}

bool LoadConfigFromIni(ProxyConfig& cfg) {
  std::wstring path = GetConfigPath();
  wchar_t buf[512];

  GetPrivateProfileStringW(L"Proxy", L"Type", L"SOCKS5", buf, 512, path.c_str());
  cfg.type = (_wcsicmp(buf, L"HTTP") == 0) ? ProxyType::HTTP : ProxyType::SOCKS5;

  GetPrivateProfileStringW(L"Proxy", L"Host", L"", buf, 512, path.c_str());
  cfg.host = buf;

  cfg.port = GetPrivateProfileIntW(L"Proxy", L"Port", 1080, path.c_str());

  cfg.useAuth = GetPrivateProfileIntW(L"Proxy", L"UseAuth", 0, path.c_str()) != 0;

  GetPrivateProfileStringW(L"Proxy", L"Username", L"", buf, 512, path.c_str());
  cfg.username = buf;

  GetPrivateProfileStringW(L"Proxy", L"Password", L"", buf, 512, path.c_str());
  cfg.password = buf;

  return true;
}

// Shared memory for hook

static HANDLE g_hMapFile = nullptr;
static LPVOID g_pView = nullptr;

bool WriteHookConfig(const ProxyConfig& cfg) {
  if (!g_hMapFile) {
    g_hMapFile = CreateFileMappingA(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      0, sizeof(HookConfig), HOOK_SHARED_NAME);
    if (!g_hMapFile) return false;
  }

  if (!g_pView) {
    g_pView = MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookConfig));
    if (!g_pView) return false;
  }

  HookConfig hc{};
  hc.proxyType = (int)cfg.type;
  hc.port = cfg.port;
  hc.useAuth = cfg.useAuth ? 1 : 0;

  std::string h = cfg.hostA();
  std::string u = cfg.usernameA();
  std::string p = cfg.passwordA();

  strncpy_s(hc.host, sizeof(hc.host), h.c_str(), _TRUNCATE);
  strncpy_s(hc.username, sizeof(hc.username), u.c_str(), _TRUNCATE);
  strncpy_s(hc.password, sizeof(hc.password), p.c_str(), _TRUNCATE);

  memcpy(g_pView, &hc, sizeof(HookConfig));
  return true;
}

bool ReadHookConfig(HookConfig& out) {
  HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, HOOK_SHARED_NAME);
  if (!hMap) return false;

  LPVOID view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(HookConfig));
  if (!view) {
    CloseHandle(hMap);
    return false;
  }

  memcpy(&out, view, sizeof(HookConfig));
  UnmapViewOfFile(view);
  CloseHandle(hMap);
  return true;
}

void ClearHookConfig() {
  if (g_pView) { UnmapViewOfFile(g_pView); g_pView = nullptr; }
  if (g_hMapFile) { CloseHandle(g_hMapFile); g_hMapFile = nullptr; }
}
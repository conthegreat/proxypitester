// ProxyPi Tester — modern, branding-aligned proxy connectivity + speed test tool
// Companion to the full ProxyTools router app (left intact).
// Branding colors from proxypi.co.uk

#include "common.h"
#include "proxy_client.h"
#include "local_bridge.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- App version & update feed (public GitHub — no credentials required) ----
// Bump APP_VERSION when you ship a new build. Host update.json on a public repo/raw URL.
static const wchar_t* APP_VERSION = L"1.2.2";
static const wchar_t* APP_NAME = L"ProxyPiTester";
// Primary: simple JSON on raw.githubusercontent.com
// Fallback: GitHub Releases API for the same repo
static const wchar_t* UPDATE_JSON_HOST = L"raw.githubusercontent.com";
static const wchar_t* UPDATE_JSON_PATH = L"/conthegreat/proxypitester/main/update.json";
static const wchar_t* GITHUB_API_HOST = L"api.github.com";
static const wchar_t* GITHUB_API_PATH = L"/repos/conthegreat/proxypitester/releases/latest";
static const wchar_t* DEFAULT_DOWNLOAD_URL = L"https://github.com/conthegreat/proxypitester/releases/latest";
static const wchar_t* WEBSITE_URL = L"https://proxypi.co.uk";

// Open a URL in the default browser. Direct .zip links are unreliable with ShellExecute;
// fall back to the releases page. Returns true if ShellExecute reported success.
static bool OpenUrlInDefaultBrowser(HWND hwnd, const std::wstring& urlIn) {
  std::wstring url = urlIn;
  while (!url.empty() && (url.back() == L' ' || url.back() == L'\t' || url.back() == L'\r' || url.back() == L'\n'))
    url.pop_back();
  if (url.empty()) url = DEFAULT_DOWNLOAD_URL;

  // Prefer a browsable release page over a raw asset URL (ShellExecute often
  // fails silently or mis-handles direct .zip download links).
  std::wstring try1 = url;
  std::wstring try2 = DEFAULT_DOWNLOAD_URL;
  bool looksLikeAsset = (url.find(L"/download/") != std::wstring::npos);
  if (!looksLikeAsset && url.size() > 4) {
    looksLikeAsset = (_wcsicmp(url.c_str() + (url.size() - 4), L".zip") == 0);
  }
  if (looksLikeAsset) {
    try1 = DEFAULT_DOWNLOAD_URL;
    try2 = url;
  }

  auto tryOpen = [&](const std::wstring& u) -> bool {
    if (u.empty()) return false;
    // ShellExecute returns value > 32 on success
    INT_PTR rc = (INT_PTR)ShellExecuteW(hwnd, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (rc > 32) return true;
    // Fallback: cmd start (handles more association edge cases)
    std::wstring params = L"/c start \"\" \"";
    params += u;
    params += L"\"";
    rc = (INT_PTR)ShellExecuteW(hwnd, L"open", L"cmd.exe", params.c_str(), nullptr, SW_HIDE);
    return rc > 32;
  };

  if (tryOpen(try1)) return true;
  if (try1 != try2 && tryOpen(try2)) return true;
  if (try1 != DEFAULT_DOWNLOAD_URL && try2 != DEFAULT_DOWNLOAD_URL && tryOpen(DEFAULT_DOWNLOAD_URL))
    return true;
  return false;
}
// Production website JSON API (proxies to internal RADIUS API)
static const wchar_t* DESKTOP_API_HOST = L"proxypi.co.uk";
static const wchar_t* DESKTOP_LOGIN_PATH = L"/api/desktop/login";

// ---- ProxyPi brand palette (from proxypi.co.uk CSS) ----
static const COLORREF COL_BG         = RGB(15, 23, 42);     // #0f172a
static const COLORREF COL_BG_DEEP    = RGB(2, 6, 23);       // #020617
static const COLORREF COL_CARD       = RGB(17, 24, 39);     // #111827
static const COLORREF COL_PRIMARY    = RGB(0, 188, 212);    // #00bcd4
static const COLORREF COL_PRIMARY_D  = RGB(0, 159, 181);    // #009fb5
static const COLORREF COL_PRIMARY_DD = RGB(0, 143, 163);    // #008fa3
static const COLORREF COL_TEXT       = RGB(248, 250, 252);  // #f8fafc
static const COLORREF COL_TEXT_BODY  = RGB(226, 232, 240);  // #e2e8f0
static const COLORREF COL_TEXT_DIM   = RGB(148, 163, 184);  // #94a3b8
static const COLORREF COL_BORDER     = RGB(30, 41, 59);     // slate-800-ish
static const COLORREF COL_SUCCESS    = RGB(34, 197, 94);    // green-500
static const COLORREF COL_ERROR      = RGB(239, 68, 68);    // red-500
static const COLORREF COL_INDIGO     = RGB(99, 102, 241);   // #6366f1 accent glow

// Control IDs
#define ID_COMBO_TYPE   2001
#define ID_EDIT_HOST    2002
#define ID_EDIT_PORT    2003
#define ID_CHECK_AUTH   2004
#define ID_EDIT_USER    2005
#define ID_EDIT_PASS    2006
#define ID_BTN_TEST     2007
#define ID_BTN_SPEED    2008
#define ID_BTN_SAVE     2009
#define ID_BTN_LOAD     2010
#define ID_BTN_SITE     2011
#define ID_EDIT_RESULT  2012
#define ID_STATIC_STATUS 2013
#define ID_EDIT_ACCT_EMAIL 2014
#define ID_EDIT_ACCT_PASS  2015
#define ID_BTN_LOGIN       2016
#define ID_BTN_DEV_CHROME  2017
#define ID_STATIC_DEV_PATH 2018
#define ID_BTN_BROWSE_CHROME 2019
#define ID_BTN_DEV_RUNELITE  2020
#define ID_BTN_DEV_JAGEX     2021
#define ID_BTN_DEV_EDGE      2022
#define ID_BTN_DEV_FIREFOX   2023
#define ID_BTN_SESS_REFRESH  2024
#define ID_LIST_SESSIONS     2025
#define ID_COMBO_APPS        2026
#define ID_BTN_LAUNCH_APP    2027
#define ID_EDIT_OPEN_URL     2028
#define ID_BTN_OPEN_URL      2029
#define ID_BTN_COPY_IP       2030
#define ID_BTN_BROWSE_APP    2031

// Menu IDs
#define IDM_FILE_EXIT       3001
#define IDM_HELP_UPDATE     3010
#define IDM_HELP_WEBSITE    3011
#define IDM_HELP_ABOUT      3012
#define IDM_HELP_APPROUTE   3013
#define IDM_HELP_SET_CHROME 3014
#define IDM_HELP_OPEN_CHROME 3015
#define IDM_HELP_CLEAR_CHROME 3016
#define IDM_HELP_OPEN_RUNELITE 3017
#define IDM_HELP_OPEN_JAGEX  3018
#define IDM_HELP_OPEN_EDGE   3019
#define IDM_HELP_OPEN_FIREFOX 3020

// Custom messages from worker threads
#define WM_APP_TEST_DONE    (WM_APP + 10)
#define WM_APP_SPEED_DONE   (WM_APP + 11)
#define WM_APP_UPDATE_DONE  (WM_APP + 12)
#define WM_APP_LOGIN_DONE   (WM_APP + 13)
#define WM_APP_ROUTE_DONE   (WM_APP + 20)

#define TIMER_SESSIONS  77

static HWND hMain = nullptr;
static HWND hComboType = nullptr;
static HWND hHost = nullptr, hPort = nullptr;
static HWND hCheckAuth = nullptr;
static HWND hUser = nullptr, hPass = nullptr;
static HWND hBtnTest = nullptr, hBtnSpeed = nullptr;
static HWND hBtnSave = nullptr, hBtnLoad = nullptr, hBtnSite = nullptr;
static HWND hResult = nullptr;
static HWND hStatus = nullptr;
static HWND hAcctEmail = nullptr, hAcctPass = nullptr, hBtnLogin = nullptr;
static HWND hBtnDevChrome = nullptr;   // hidden legacy; apps use dropdown
static HWND hBtnDevEdge = nullptr;
static HWND hBtnDevFirefox = nullptr;
static HWND hBtnDevRuneLite = nullptr;
static HWND hBtnDevJagex = nullptr;
static HWND hBtnSessRefresh = nullptr;
static HWND hBtnBrowseChrome = nullptr;
static HWND hChromePathLabel = nullptr;
static HWND hSessionList = nullptr;
static HWND hComboApps = nullptr;
static HWND hBtnLaunchApp = nullptr;
static HWND hEditOpenUrl = nullptr;
static HWND hBtnOpenUrl = nullptr;
static HWND hBtnCopyIp = nullptr;
static HWND hBtnBrowseApp = nullptr;
static std::wstring g_chromePath; // empty = auto-detect
static std::wstring g_customAppPath; // last browsed Chromium-like exe
static int g_lastAppSel = 0;         // combo index remembered in ini

static HFONT hFontUi = nullptr;
static HFONT hFontTitle = nullptr;
static HFONT hFontSmall = nullptr;
static HFONT hFontMono = nullptr;
static HBRUSH hBrushBg = nullptr;
static HBRUSH hBrushCard = nullptr;
static HBRUSH hBrushInput = nullptr;
static HBRUSH hBrushPrimary = nullptr;

static bool g_busy = false;
static std::wstring g_statusText = L"Ready - enter your ProxyPi credentials and test.";
static COLORREF g_statusColor = COL_TEXT_DIM;

// Result snapshot for painted metric cards
static bool g_hasResult = false;
static bool g_lastOk = false;
static std::wstring g_lastIp;
static int g_lastLatency = 0;
static double g_lastMbps = -1.0;  // -1 = not measured
static std::wstring g_lastDetail;
static bool g_showedDnsTip = false;

static void AppendResult(const std::wstring& text); // forward decl

static void MaybeAppendDnsLatencyTip() {
  if (g_showedDnsTip) return;
  g_showedDnsTip = true;
  AppendResult(L"  (First test is often slower: DNS + cold proxy path.)");
}

// Ports from last account login (or sensible defaults) so Type can swap SOCKS <-> HTTP
static int g_socksPort = 18721;
static int g_httpPort = 58920;

static void ApplyPortForSelectedType() {
  if (!hComboType || !hPort) return;
  int sel = (int)SendMessageW(hComboType, CB_GETCURSEL, 0, 0);
  if (sel < 0) sel = 0;
  // Ensure we always have a usable pair after login
  int socks = (g_socksPort > 0) ? g_socksPort : 18721;
  int http  = (g_httpPort  > 0) ? g_httpPort  : 58920;
  int port = (sel == 1) ? http : socks;
  SetWindowTextW(hPort, std::to_wstring(port).c_str());
}

// Call when both ports are known (login success) — stores pair and refreshes Port field once
static void SetPortPairAndRefresh(int socksPort, int httpPort) {
  if (socksPort > 0) g_socksPort = socksPort;
  if (httpPort > 0)  g_httpPort  = httpPort;
  // If API only returned one, keep the other default so swapping still works
  if (g_socksPort <= 0) g_socksPort = 18721;
  if (g_httpPort  <= 0) g_httpPort  = 58920;
  ApplyPortForSelectedType();
}

// Default client size (window is resizable / maximizable; min enforced in WM_GETMINMAXINFO)
static const int DEFAULT_CLIENT_W = 900;
static const int DEFAULT_CLIENT_H = 820;
static const int MIN_CLIENT_W = 640;
static const int MIN_CLIENT_H = 700;
static const int PAD = 24;
// Side-by-side account | config when client is at least this wide
static const int SIDE_BY_SIDE_MIN_W = 860;

// Fixed control widths (do not stretch with window)
static const int BTN_PRIMARY_W = 140;
static const int BTN_SECONDARY_W = 96;
static const int BTN_LOGIN_W = 260;
static const int BTN_LAUNCH_W = 110;
static const int BTN_REFRESH_W = 96;
static const int COMBO_APPS_W = 230;
static const int BTN_COPY_W = 100;
static const int BTN_URL_W = 100;

// Runtime layout (recomputed on resize)
struct UiLayout {
  int clientW = DEFAULT_CLIENT_W;
  int clientH = DEFAULT_CLIENT_H;
  int cardX = 20;
  int cardW = DEFAULT_CLIENT_W - 40;
  bool sideBySide = false;
  // Account card rect
  int acctLeft = 20, acctTop = 112, acctW = 0, acctBot = 250;
  // Config card rect
  int cfgLeft = 20, cfgTop = 262, cfgW = 0, cfgBot = 470;
  int metricsY = 482;
  int testBtnY = 572, secBtnY = 624;
  int routeTop = 670, routeBot = 900;
  int resTop = 912, resBot = 1000;
  int statusY = 1010, footerY = 1055;
};
static UiLayout g_lay;

// Compatibility aliases used by older paint code paths during transition
#define CARD_X (g_lay.cardX)
#define CARD_W (g_lay.cardW)
#define CLIENT_W (g_lay.clientW)
#define CLIENT_H (g_lay.clientH)
#define ROUTE_CARD_TOP (g_lay.routeTop)
#define ROUTE_CARD_BOT (g_lay.routeBot)
#define RES_CARD_TOP (g_lay.resTop)

// Field labels (moved on resize)
static HWND hLblAcctEmail = nullptr, hLblAcctPass = nullptr;
static HWND hLblType = nullptr, hLblHost = nullptr, hLblPort = nullptr;
static HWND hLblUser = nullptr, hLblPass = nullptr;

// ---- helpers ----
static std::wstring GetExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s = path;
  size_t p = s.find_last_of(L"\\/");
  if (p != std::wstring::npos) s = s.substr(0, p);
  return s;
}

static std::wstring GetIniPath() {
  return GetExeDir() + L"\\ProxyPiTester.ini";
}

static bool SaveIni(const ProxyConfig& cfg) {
  std::wstring path = GetIniPath();
  WritePrivateProfileStringW(L"Proxy", L"Type", cfg.type == ProxyType::SOCKS5 ? L"SOCKS5" : L"HTTP", path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Host", cfg.host.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Port", std::to_wstring(cfg.port).c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"UseAuth", cfg.useAuth ? L"1" : L"0", path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Username", cfg.username.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Password", cfg.password.c_str(), path.c_str());
  WritePrivateProfileStringW(L"AppRoute", L"ChromePath", g_chromePath.c_str(), path.c_str());
  return true;
}

static bool LoadIni(ProxyConfig& cfg) {
  std::wstring path = GetIniPath();
  wchar_t buf[512];
  GetPrivateProfileStringW(L"Proxy", L"Type", L"SOCKS5", buf, 512, path.c_str());
  cfg.type = (_wcsicmp(buf, L"HTTP") == 0) ? ProxyType::HTTP : ProxyType::SOCKS5;
  GetPrivateProfileStringW(L"Proxy", L"Host", L"", buf, 512, path.c_str());
  cfg.host = buf;
  cfg.port = GetPrivateProfileIntW(L"Proxy", L"Port", 18721, path.c_str());
  cfg.useAuth = GetPrivateProfileIntW(L"Proxy", L"UseAuth", 1, path.c_str()) != 0;
  GetPrivateProfileStringW(L"Proxy", L"Username", L"", buf, 512, path.c_str());
  cfg.username = buf;
  GetPrivateProfileStringW(L"Proxy", L"Password", L"", buf, 512, path.c_str());
  cfg.password = buf;
  wchar_t chromeBuf[MAX_PATH] = {};
  GetPrivateProfileStringW(L"AppRoute", L"ChromePath", L"", chromeBuf, MAX_PATH, path.c_str());
  g_chromePath = chromeBuf;
  return !cfg.host.empty();
}

static void SaveChromePathIni() {
  WritePrivateProfileStringW(L"AppRoute", L"ChromePath", g_chromePath.c_str(), GetIniPath().c_str());
}

static std::wstring EffectiveChromePath() {
  if (!g_chromePath.empty() && GetFileAttributesW(g_chromePath.c_str()) != INVALID_FILE_ATTRIBUTES)
    return g_chromePath;
  return FindDefaultChromePath();
}

static std::wstring ShortenPathForUi(const std::wstring& p, size_t maxChars = 52) {
  if (p.empty()) return L"Chrome: not found (set path in Help > App routing)";
  if (p.size() <= maxChars) return L"Chrome: " + p;
  // keep drive + ... + tail
  std::wstring tail = p.substr(p.size() - (maxChars - 12));
  size_t slash = tail.find_first_of(L"\\/");
  if (slash != std::wstring::npos && slash + 1 < tail.size()) tail = tail.substr(slash + 1);
  return L"Chrome: " + p.substr(0, 3) + L"..." + tail;
}

static void RefreshChromePathLabel() {
  if (!hChromePathLabel) return;
  int bp = GetLocalAuthBridgePort();
  std::wstring line;
  if (bp > 0) {
    line = L"Bridge UP  127.0.0.1:" + std::to_wstring(bp);
    if (g_hasResult && !g_lastIp.empty())
      line += L"  |  Exit IP " + g_lastIp;
  } else {
    line = L"Bridge off - open an app to start";
    if (g_hasResult && !g_lastIp.empty())
      line += L"  |  Last exit " + g_lastIp;
  }
  if (IsRuneLiteSocksWrapActive())
    line += L"  |  RuneLite wrap armed";
  SetWindowTextW(hChromePathLabel, line.c_str());
}

// Prompt if app already running (Electron apps ignore new proxy flags if already open)
static bool ConfirmRelaunchIfRunning(const wchar_t* appName, const wchar_t* imageExe) {
  if (!IsImageRunning(imageExe)) return true;
  std::wstring msg = appName;
  msg += L" is already running.\r\n\r\n";
  msg += L"It will not pick up the proxy until you fully quit it (including tray icons),\r\n";
  msg += L"then open it again from ProxyPiTester.\r\n\r\n";
  msg += L"Launch anyway?";
  return MessageBoxW(hMain, msg.c_str(), L"App already running",
                     MB_YESNO | MB_ICONWARNING) == IDYES;
}

static void SaveAppRoutePrefs() {
  std::wstring path = GetIniPath();
  WritePrivateProfileStringW(L"AppRoute", L"LastAppSel",
                             std::to_wstring(g_lastAppSel).c_str(), path.c_str());
  wchar_t url[1024] = {};
  if (hEditOpenUrl) GetWindowTextW(hEditOpenUrl, url, 1024);
  WritePrivateProfileStringW(L"AppRoute", L"LastUrl", url, path.c_str());
  if (!g_customAppPath.empty())
    WritePrivateProfileStringW(L"AppRoute", L"CustomApp", g_customAppPath.c_str(), path.c_str());
}

static void LoadAppRoutePrefs() {
  std::wstring path = GetIniPath();
  g_lastAppSel = GetPrivateProfileIntW(L"AppRoute", L"LastAppSel", 0, path.c_str());
  wchar_t url[1024] = {}, custom[MAX_PATH] = {};
  GetPrivateProfileStringW(L"AppRoute", L"LastUrl", L"https://ifconfig.me", url, 1024, path.c_str());
  GetPrivateProfileStringW(L"AppRoute", L"CustomApp", L"", custom, MAX_PATH, path.c_str());
  g_customAppPath = custom;
  if (hEditOpenUrl && url[0]) SetWindowTextW(hEditOpenUrl, url);
  if (hComboApps) {
    int count = (int)SendMessageW(hComboApps, CB_GETCOUNT, 0, 0);
    if (g_lastAppSel >= 0 && g_lastAppSel < count)
      SendMessageW(hComboApps, CB_SETCURSEL, g_lastAppSel, 0);
  }
}

// Fixed-width cell for mono listbox columns (Consolas)
static std::wstring PadCell(const std::wstring& s, size_t width, bool rightAlign = false) {
  if (s.size() >= width) return s.substr(0, width);
  std::wstring pad(width - s.size(), L' ');
  return rightAlign ? (pad + s) : (s + pad);
}

static void RefreshSessionList() {
  if (!hSessionList) return;
  SendMessageW(hSessionList, LB_RESETCONTENT, 0, 0);
  int bp = GetLocalAuthBridgePort();

  // Status lines (full width, not columns)
  if (bp > 0) {
    std::wstring head = L"Bridge UP   127.0.0.1:" + std::to_wstring(bp);
    if (g_hasResult && !g_lastIp.empty())
      head += L"   Exit IP " + g_lastIp;
    else
      head += L"   (run Test Proxy for exit IP)";
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)head.c_str());
  } else {
    std::wstring head = L"Bridge OFF  - open an app to start";
    if (g_hasResult && !g_lastIp.empty())
      head += L"   Last exit " + g_lastIp;
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)head.c_str());
  }
  if (IsRuneLiteSocksWrapActive()) {
    std::wstring w = L"Wrap        RuneLite SOCKS armed on port " +
                     std::to_wstring(GetRuneLiteSocksWrapPort());
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)w.c_str());
  }

  auto sessions = GetRoutedAppSessions();
  if (sessions.empty()) {
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)L"");
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)L"  (no routed apps running)");
    return;
  }

  // APP(14) PID(8) PROCS(5) CONNS(6) BRIDGE(6) METHOD
  SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)L"");
  {
    std::wstring hdr =
      PadCell(L"APP", 14) + L" " +
      PadCell(L"PID", 8, true) + L" " +
      PadCell(L"PROCS", 5, true) + L" " +
      PadCell(L"CONNS", 6, true) + L" " +
      PadCell(L"BRIDGE", 6, true) + L"  " +
      L"METHOD";
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)hdr.c_str());
    SendMessageW(hSessionList, LB_ADDSTRING, 0,
      (LPARAM)L"-------------- -------- ----- ------ ------  ----------------");
  }

  for (const auto& s : sessions) {
    std::wstring line =
      PadCell(s.name, 14) + L" " +
      PadCell(std::to_wstring(s.pid), 8, true) + L" " +
      PadCell(std::to_wstring(s.processCount > 0 ? s.processCount : 1), 5, true) + L" " +
      PadCell(s.connCount > 0 ? std::to_wstring(s.connCount) : L"-", 6, true) + L" " +
      PadCell(s.bridgePort > 0 ? std::to_wstring(s.bridgePort) : L"-", 6, true) + L"  " +
      (s.method.empty() ? L"-" : s.method);
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
  }
}

static void SetStatus(const std::wstring& text, COLORREF color = COL_TEXT_DIM); // fwd
static ProxyConfig ReadConfigFromUI(); // fwd

// Require proxy host before app routing
static bool RequireProxyForRoute(ProxyConfig& cfg) {
  cfg = ReadConfigFromUI();
  if (cfg.host.empty()) {
    SetStatus(L"Configure or login to load a proxy first.", COL_ERROR);
    MessageBoxW(hMain,
      L"Load a proxy first (Login and Load My Proxy), or enter host/port manually.",
      L"App routing", MB_ICONWARNING | MB_OK);
    return false;
  }
  return true;
}

static void PostRouteResult(bool ok, const std::wstring& msg) {
  auto* heap = new std::wstring(msg);
  PostMessageW(hMain, WM_APP_ROUTE_DONE, ok ? 1 : 0, (LPARAM)heap);
}

static void SetStatus(const std::wstring& text, COLORREF color) {
  // Keep status short so it fits the footer strip (full detail is in the Results log).
  std::wstring t = text;
  if (t.size() > 90) {
    t = t.substr(0, 87) + L"...";
  }
  g_statusText = t;
  g_statusColor = color;
  if (hStatus) SetWindowTextW(hStatus, t.c_str());
  if (hMain) InvalidateRect(hMain, nullptr, FALSE);
}

static void SetBusy(bool busy) {
  g_busy = busy;
  EnableWindow(hBtnTest, !busy);
  EnableWindow(hBtnSpeed, !busy);
  EnableWindow(hBtnSave, !busy);
  EnableWindow(hBtnLoad, !busy);
  if (hBtnLogin) EnableWindow(hBtnLogin, !busy);
  // App routing buttons stay enabled so multiple apps can be launched together
  if (hBtnBrowseChrome) EnableWindow(hBtnBrowseChrome, !busy);
}

static ProxyConfig ReadConfigFromUI() {
  ProxyConfig cfg;
  int sel = (int)SendMessageW(hComboType, CB_GETCURSEL, 0, 0);
  cfg.type = (sel == 1) ? ProxyType::HTTP : ProxyType::SOCKS5;

  wchar_t buf[512];
  GetWindowTextW(hHost, buf, 512);
  cfg.host = buf;
  GetWindowTextW(hPort, buf, 512);
  cfg.port = _wtoi(buf);
  if (cfg.port <= 0) cfg.port = (cfg.type == ProxyType::SOCKS5 ? 1080 : 8080);

  cfg.useAuth = SendMessageW(hCheckAuth, BM_GETCHECK, 0, 0) == BST_CHECKED;
  GetWindowTextW(hUser, buf, 512);
  cfg.username = buf;
  GetWindowTextW(hPass, buf, 512);
  cfg.password = buf;
  return cfg;
}

static void WriteConfigToUI(const ProxyConfig& cfg) {
  SendMessageW(hComboType, CB_SETCURSEL, cfg.type == ProxyType::HTTP ? 1 : 0, 0);
  SetWindowTextW(hHost, cfg.host.c_str());
  SetWindowTextW(hPort, std::to_wstring(cfg.port).c_str());
  // Keep port pair in sync so Type can swap later
  if (cfg.type == ProxyType::HTTP) {
    if (cfg.port > 0) g_httpPort = cfg.port;
  } else {
    if (cfg.port > 0) g_socksPort = cfg.port;
  }
  SendMessageW(hCheckAuth, BM_SETCHECK, cfg.useAuth ? BST_CHECKED : BST_UNCHECKED, 0);
  SetWindowTextW(hUser, cfg.username.c_str());
  SetWindowTextW(hPass, cfg.password.c_str());
  BOOL en = cfg.useAuth ? TRUE : FALSE;
  EnableWindow(hUser, en);
  EnableWindow(hPass, en);
}

static void AppendResult(const std::wstring& text) {
  if (!hResult) return;
  int len = GetWindowTextLengthW(hResult);
  SendMessageW(hResult, EM_SETSEL, (WPARAM)len, (LPARAM)len);
  SendMessageW(hResult, EM_REPLACESEL, FALSE, (LPARAM)(text + L"\r\n").c_str());
}

// ---- Version / update helpers ----
struct VersionTriple {
  int major = 0, minor = 0, patch = 0;
};

static VersionTriple ParseVersion(const std::wstring& s) {
  VersionTriple v{};
  std::wstring t = s;
  // strip leading 'v' or 'V'
  if (!t.empty() && (t[0] == L'v' || t[0] == L'V')) t = t.substr(1);
  // keep only digits and dots
  std::wstring clean;
  for (wchar_t c : t) {
    if ((c >= L'0' && c <= L'9') || c == L'.') clean.push_back(c);
    else break;
  }
  int parts[3] = {0, 0, 0};
  int idx = 0;
  std::wstring num;
  for (size_t i = 0; i <= clean.size() && idx < 3; ++i) {
    if (i == clean.size() || clean[i] == L'.') {
      if (!num.empty()) parts[idx++] = _wtoi(num.c_str());
      num.clear();
    } else {
      num.push_back(clean[i]);
    }
  }
  v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2];
  return v;
}

static int CompareVersion(const VersionTriple& a, const VersionTriple& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// Minimal JSON string value extractor: "key" : "value"
static bool JsonGetString(const std::string& json, const char* key, std::string& out) {
  out.clear();
  std::string pat = std::string("\"") + key + "\"";
  size_t p = json.find(pat);
  if (p == std::string::npos) return false;
  p = json.find(':', p + pat.size());
  if (p == std::string::npos) return false;
  p = json.find('"', p + 1);
  if (p == std::string::npos) return false;
  size_t start = p + 1;
  size_t end = start;
  while (end < json.size()) {
    if (json[end] == '\\' && end + 1 < json.size()) { end += 2; continue; }
    if (json[end] == '"') break;
    ++end;
  }
  if (end >= json.size()) return false;
  out = json.substr(start, end - start);
  // Unescape a few common sequences
  std::string u;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\\' && i + 1 < out.size()) {
      char n = out[i + 1];
      if (n == 'n') { u.push_back('\n'); ++i; }
      else if (n == 'r') { u.push_back('\r'); ++i; }
      else if (n == 't') { u.push_back('\t'); ++i; }
      else if (n == '"' || n == '\\' || n == '/') { u.push_back(n); ++i; }
      else u.push_back(out[i]);
    } else {
      u.push_back(out[i]);
    }
  }
  out = u;
  return true;
}

static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

// Escape a string for JSON body
static std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back((char)c); }
    else if (c == '\n') { o += "\\n"; }
    else if (c == '\r') { o += "\\r"; }
    else if (c == '\t') { o += "\\t"; }
    else o.push_back((char)c);
  }
  return o;
}

static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

// Strict "key": number extractor (avoids matching digits inside other keys/values)
static bool JsonGetNumber(const std::string& json, const char* key, int& out) {
  std::string needle = std::string("\"") + key + "\"";
  size_t pos = 0;
  while ((pos = json.find(needle, pos)) != std::string::npos) {
    size_t k = pos + needle.size();
    while (k < json.size() && (json[k] == ' ' || json[k] == '\t' || json[k] == '\r' || json[k] == '\n')) k++;
    if (k >= json.size() || json[k] != ':') { ++pos; continue; }
    ++k;
    while (k < json.size() && (json[k] == ' ' || json[k] == '\t' || json[k] == '\r' || json[k] == '\n')) k++;
    if (k < json.size() && (isdigit((unsigned char)json[k]) || json[k] == '-')) {
      out = atoi(json.c_str() + k);
      return true;
    }
    ++pos;
  }
  return false;
}

// Extract a top-level object value for key "proxy": { ... }
static std::string JsonExtractObject(const std::string& json, const char* key) {
  std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return {};
  p = json.find('{', p + needle.size());
  if (p == std::string::npos) return {};
  int depth = 0;
  for (size_t i = p; i < json.size(); ++i) {
    if (json[i] == '{') depth++;
    else if (json[i] == '}') {
      depth--;
      if (depth == 0) return json.substr(p, i - p + 1);
    }
  }
  return {};
}

static bool JsonGetBool(const std::string& json, const char* key, bool& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t p = json.find(pat);
  if (p == std::string::npos) return false;
  p = json.find(':', p + pat.size());
  if (p == std::string::npos) return false;
  size_t t = json.find("true", p);
  size_t f = json.find("false", p);
  if (t != std::string::npos && (f == std::string::npos || t < f) && t < p + 12) {
    out = true; return true;
  }
  if (f != std::string::npos && f < p + 12) {
    out = false; return true;
  }
  return false;
}

// HTTPS GET via WinHTTP (for public update feeds)
static bool HttpGetHttps(const wchar_t* host, const wchar_t* path, std::string& outBody, std::wstring& err,
                         const wchar_t* userAgent = L"ProxyPiTester/1.0") {
  outBody.clear();
  HINTERNET hSession = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) { err = L"WinHttpOpen failed"; return false; }

  DWORD timeout = 12000;
  WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

  HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    err = L"WinHttpConnect failed";
    WinHttpCloseHandle(hSession);
    return false;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    err = L"WinHttpOpenRequest failed";
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  // GitHub API requires a User-Agent
  std::wstring headers = L"Accept: application/json\r\n";
  BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (!ok || !WinHttpReceiveResponse(hRequest, nullptr)) {
    err = L"Update server request failed (network or HTTPS)";
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  DWORD status = 0, statusSize = sizeof(status);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
    if (avail == 0) break;
    std::vector<char> buf(avail + 1);
    DWORD read = 0;
    if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
    outBody.append(buf.data(), read);
    if (outBody.size() > 512 * 1024) break; // safety
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  if (status != 200) {
    err = L"Update check HTTP status " + std::to_wstring(status);
    return false;
  }
  if (outBody.empty()) {
    err = L"Empty response from update server";
    return false;
  }
  return true;
}

// HTTPS POST JSON body
static bool HttpPostHttpsJson(const wchar_t* host, const wchar_t* path, const std::string& jsonBody,
                              std::string& outBody, int& outStatus, std::wstring& err) {
  outBody.clear();
  outStatus = 0;
  HINTERNET hSession = WinHttpOpen(L"ProxyPiTester/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) { err = L"WinHttpOpen failed"; return false; }
  DWORD timeout = 20000;
  WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

  HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    err = L"Cannot reach " + std::wstring(host);
    WinHttpCloseHandle(hSession);
    return false;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    err = L"WinHttpOpenRequest failed";
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
  BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L,
                               (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(),
                               (DWORD)jsonBody.size(), 0);
  if (!ok || !WinHttpReceiveResponse(hRequest, nullptr)) {
    err = L"Login request failed (network or HTTPS)";
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
  }

  DWORD status = 0, statusSize = sizeof(status);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
  outStatus = (int)status;

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
    if (avail == 0) break;
    std::vector<char> buf(avail + 1);
    DWORD read = 0;
    if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
    outBody.append(buf.data(), read);
    if (outBody.size() > 256 * 1024) break;
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return true;
}

struct DesktopLoginResult {
  bool ok = false;
  std::wstring message;
  std::wstring host;
  int socksPort = 0;
  int httpPort = 0;
  std::wstring proxyUser;
  std::wstring proxyPass;
  std::wstring email;
  double usedMb = 0;
  double limitMb = 0;
};

static DesktopLoginResult PerformDesktopLogin(const std::wstring& email, const std::wstring& password) {
  DesktopLoginResult r;
  std::string bodyJson = std::string("{\"email\":\"") + JsonEscape(WideToUtf8(email)) +
                         "\",\"password\":\"" + JsonEscape(WideToUtf8(password)) + "\"}";
  std::string resp;
  int status = 0;
  std::wstring err;
  if (!HttpPostHttpsJson(DESKTOP_API_HOST, DESKTOP_LOGIN_PATH, bodyJson, resp, status, err)) {
    r.message = err;
    return r;
  }

  bool okFlag = false;
  JsonGetBool(resp, "ok", okFlag);
  std::string errA, hostA, userA, passA;
  JsonGetString(resp, "error", errA);

  // Parse proxy object first so both ports are read in one go (not mixed with usage fields)
  std::string proxyObj = JsonExtractObject(resp, "proxy");
  const std::string& proxyJson = !proxyObj.empty() ? proxyObj : resp;

  JsonGetString(proxyJson, "host", hostA);
  JsonGetString(proxyJson, "username", userA);
  JsonGetString(proxyJson, "password", passA);
  int socks = 0, http = 0;
  JsonGetNumber(proxyJson, "socks_port", socks);
  JsonGetNumber(proxyJson, "http_port", http);
  // Fallback: some older payloads used "port" for SOCKS only
  if (socks <= 0) JsonGetNumber(proxyJson, "port", socks);

  if (!okFlag || status != 200) {
    r.ok = false;
    if (!errA.empty()) r.message = Utf8ToWide(errA);
    else r.message = L"Login failed (HTTP " + std::to_wstring(status) + L")";
    return r;
  }

  if (hostA.empty() || socks <= 0 || userA.empty() || passA.empty()) {
    r.ok = false;
    r.message = L"Login OK but proxy details incomplete from server.";
    return r;
  }
  // HTTP port may be missing on some nodes — keep a usable default so Type swap still works
  if (http <= 0) http = 58920;

  r.ok = true;
  r.email = email;
  r.host = Utf8ToWide(hostA);
  r.socksPort = socks;
  r.httpPort = http; // both always set on success
  r.proxyUser = Utf8ToWide(userA);
  r.proxyPass = Utf8ToWide(passA);

  // optional usage numbers (may be floats in JSON - atoi still works for leading digits)
  int total = 0, thresh = 0;
  JsonGetNumber(resp, "total_mb", total);
  JsonGetNumber(resp, "threshold_mb", thresh);
  r.usedMb = total;
  r.limitMb = thresh;
  r.message = L"Loaded proxy for " + email;
  return r;
}

struct UpdateCheckResult {
  bool ok = false;           // network/parse succeeded
  bool updateAvailable = false;
  std::wstring message;
  std::wstring latestVersion;
  std::wstring downloadUrl;
  std::wstring notes;
  bool silent = false;       // true = startup check, only prompt if update exists
};

static UpdateCheckResult PerformUpdateCheck() {
  UpdateCheckResult r;
  r.downloadUrl = DEFAULT_DOWNLOAD_URL;
  r.latestVersion = APP_VERSION;

  std::string body;
  std::wstring err;
  std::string versionA, urlA, notesA;

  // 1) Prefer simple update.json (easy for you to edit on GitHub)
  if (HttpGetHttps(UPDATE_JSON_HOST, UPDATE_JSON_PATH, body, err)) {
    JsonGetString(body, "version", versionA);
    JsonGetString(body, "download_url", urlA);
    if (urlA.empty()) JsonGetString(body, "url", urlA);
    JsonGetString(body, "notes", notesA);
  } else {
    // 2) Fallback: GitHub Releases API (tag_name + html_url)
    body.clear();
    if (HttpGetHttps(GITHUB_API_HOST, GITHUB_API_PATH, body, err, L"ProxyPiTester/1.0")) {
      JsonGetString(body, "tag_name", versionA);
      JsonGetString(body, "html_url", urlA);
      JsonGetString(body, "body", notesA);
    } else {
      r.ok = false;
      r.message = L"Could not reach update server. " + err +
                  L" (Publish a public repo conthegreat/ProxyPiTester with update.json or a Release.)";
      return r;
    }
  }

  if (versionA.empty()) {
    r.ok = false;
    r.message = L"Update feed did not include a version field.";
    return r;
  }

  r.ok = true;
  r.latestVersion = Utf8ToWide(versionA);
  if (!urlA.empty()) r.downloadUrl = Utf8ToWide(urlA);
  if (!notesA.empty()) {
    r.notes = Utf8ToWide(notesA);
    if (r.notes.size() > 400) r.notes = r.notes.substr(0, 397) + L"...";
  }

  VersionTriple cur = ParseVersion(APP_VERSION);
  VersionTriple lat = ParseVersion(r.latestVersion);
  r.updateAvailable = CompareVersion(cur, lat) < 0;

  if (r.updateAvailable) {
    r.message = L"Update available: v" + r.latestVersion + L" (you have v" + std::wstring(APP_VERSION) + L")";
  } else {
    r.message = L"You are on the latest version (v" + std::wstring(APP_VERSION) + L").";
  }
  return r;
}

static void ShowAboutDialog(HWND parent) {
  std::wstring msg;
  msg += L"ProxyPiTester\r\n";
  msg += L"Version ";
  msg += APP_VERSION;
  msg += L"\r\n\r\n";
  msg += L"UK residential proxy testing and app routing for ProxyPi.\r\n";
  msg += L"Test connectivity and speed, then open browsers and apps\r\n";
  msg += L"through your assigned node (shared local SOCKS bridge).\r\n\r\n";
  msg += L"https://proxypi.co.uk\r\n";
  msg += L"Support: support@proxypi.co.uk\r\n";
  MessageBoxW(parent, msg.c_str(), L"About ProxyPiTester", MB_OK | MB_ICONINFORMATION);
}

static void ShowAppRouteHelp(HWND parent) {
  std::wstring msg;
  msg += L"App routing\r\n\r\n";
  msg += L"All apps share one local SOCKS bridge on 127.0.0.1 that authenticates\r\n";
  msg += L"to your ProxyPi node. Launch as many apps as you need at the same time.\r\n\r\n";
  msg += L"Supported apps:\r\n";
  msg += L"  Browsers  Chrome, Edge, Brave, Firefox, Opera, Vivaldi\r\n";
  msg += L"  Chat/IDE  Discord, Slack, Teams, VS Code, Cursor\r\n";
  msg += L"  Other     Postman, Thunderbird, Spotify\r\n";
  msg += L"  Gaming    RuneLite, Jagex Launcher\r\n";
  msg += L"  Custom    Browse any Chromium / Electron .exe\r\n\r\n";
  msg += L"Open URL opens a link through a proxied browser.\r\n";
  msg += L"Copy IP copies exit IP + session notes for support.\r\n";
  msg += L"Last app and URL are remembered on exit.\r\n";
  msg += L"Keep ProxyPiTester open while routed apps are running.\r\n\r\n";
  auto det = [](const std::wstring& label, const std::wstring& p) {
    return label + (p.empty() ? L"(not found)\r\n" : (p + L"\r\n"));
  };
  msg += L"Detected on this PC:\r\n";
  msg += det(L"  Chrome:      ", EffectiveChromePath());
  msg += det(L"  Edge:        ", FindDefaultEdgePath());
  msg += det(L"  Brave:       ", FindDefaultBravePath());
  msg += det(L"  Firefox:     ", FindDefaultFirefoxPath());
  msg += det(L"  Discord:     ", FindDefaultDiscordPath());
  msg += det(L"  Opera:       ", FindDefaultOperaPath());
  msg += det(L"  Vivaldi:     ", FindDefaultVivaldiPath());
  msg += det(L"  Slack:       ", FindDefaultSlackPath());
  msg += det(L"  Teams:       ", FindDefaultTeamsPath());
  msg += det(L"  VS Code:     ", FindDefaultVSCodePath());
  msg += det(L"  Cursor:      ", FindDefaultCursorPath());
  msg += det(L"  Postman:     ", FindDefaultPostmanPath());
  msg += det(L"  Thunderbird: ", FindDefaultThunderbirdPath());
  msg += det(L"  Spotify:     ", FindDefaultSpotifyPath());
  msg += det(L"  RuneLite:    ", FindDefaultRuneLiteDir());
  msg += det(L"  Jagex:       ", FindDefaultJagexLauncherPath());
  MessageBoxW(parent, msg.c_str(), L"App routing", MB_OK | MB_ICONINFORMATION);
}

static bool BrowseForChromePath(HWND parent) {
  wchar_t file[MAX_PATH] = {};
  if (!g_chromePath.empty() && g_chromePath.size() < MAX_PATH)
    wcsncpy_s(file, g_chromePath.c_str(), _TRUNCATE);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = parent;
  ofn.lpstrFilter = L"Chrome executable (chrome.exe)\0chrome.exe\0Executables (*.exe)\0*.exe\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"Select chrome.exe";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) return false;
  g_chromePath = file;
  SaveChromePathIni();
  RefreshChromePathLabel();
  AppendResult(L"Chrome path set: " + g_chromePath);
  SetStatus(L"Chrome path updated.", COL_SUCCESS);
  return true;
}

static void ClearChromePath() {
  g_chromePath.clear();
  SaveChromePathIni();
  RefreshChromePathLabel();
  AppendResult(L"Chrome path cleared (using auto-detect).");
  SetStatus(L"Chrome path: auto-detect.", COL_TEXT_DIM);
}

static HMENU CreateAppMenu() {
  HMENU hMenubar = CreateMenu();
  HMENU hFile = CreatePopupMenu();
  HMENU hHelp = CreatePopupMenu();
  HMENU hAppRoute = CreatePopupMenu();

  AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit");

  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_OPEN_CHROME, L"Open Chrome (proxied)");
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_OPEN_EDGE, L"Open Edge (proxied)");
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_OPEN_FIREFOX, L"Open Firefox (proxied)");
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_OPEN_RUNELITE, L"Open RuneLite (proxied)");
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_OPEN_JAGEX, L"Open Jagex + RuneLite wrap");
  AppendMenuW(hAppRoute, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_SET_CHROME, L"Set Chrome path...");
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_CLEAR_CHROME, L"Reset Chrome path (auto-detect)");
  AppendMenuW(hAppRoute, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hAppRoute, MF_STRING, IDM_HELP_APPROUTE, L"About app routing");

  AppendMenuW(hHelp, MF_STRING, IDM_HELP_UPDATE, L"Check for &Updates...");
  AppendMenuW(hHelp, MF_STRING, IDM_HELP_WEBSITE, L"Visit &Website");
  AppendMenuW(hHelp, MF_POPUP, (UINT_PTR)hAppRoute, L"App &routing");
  AppendMenuW(hHelp, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"&About ProxyPiTester");

  AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hFile, L"&File");
  AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hHelp, L"&Help");
  return hMenubar;
}

static void DoDevLaunchChrome() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  std::wstring chrome = EffectiveChromePath();
  if (chrome.empty()) {
    SetStatus(L"Chrome not found. Set path under Help > App routing.", COL_ERROR);
    if (MessageBoxW(hMain,
          L"chrome.exe was not found.\r\n\r\nBrowse for Chrome now?",
          L"App routing", MB_YESNO | MB_ICONWARNING) == IDYES) {
      if (!BrowseForChromePath(hMain)) return;
      chrome = EffectiveChromePath();
    }
    if (chrome.empty()) return;
  }
  SetStatus(L"Starting Chrome...", COL_PRIMARY);
  AppendResult(L"--- Open Chrome (proxied) ---");
  std::thread([cfg, chrome]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchChromeViaBridge(cfg, pid, err, chrome);
    PostRouteResult(ok, ok
      ? (L"Chrome started (PID " + std::to_wstring(pid) + L"). Shared bridge; check ifconfig.me.")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoDevLaunchEdge() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (FindDefaultEdgePath().empty()) {
    SetStatus(L"Microsoft Edge not found.", COL_ERROR);
    MessageBoxW(hMain, L"msedge.exe was not found under Program Files\\Microsoft\\Edge.",
                L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  SetStatus(L"Starting Edge...", COL_PRIMARY);
  AppendResult(L"--- Open Edge (proxied) ---");
  std::thread([cfg]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchEdgeViaBridge(cfg, pid, err);
    PostRouteResult(ok, ok
      ? (L"Edge started (PID " + std::to_wstring(pid) + L"). Shared bridge; check ifconfig.me.")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoDevLaunchFirefox() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (FindDefaultFirefoxPath().empty()) {
    SetStatus(L"Firefox not found.", COL_ERROR);
    MessageBoxW(hMain,
      L"firefox.exe was not found.\r\nInstall Mozilla Firefox from mozilla.org.",
      L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  SetStatus(L"Starting Firefox...", COL_PRIMARY);
  AppendResult(L"--- Open Firefox (proxied) ---");
  std::thread([cfg]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchFirefoxViaBridge(cfg, pid, err);
    PostRouteResult(ok, ok
      ? (L"Firefox started (PID " + std::to_wstring(pid) + L"). Temp profile -> local SOCKS bridge.")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoDevLaunchRuneLite() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (FindDefaultRuneLiteDir().empty()) {
    SetStatus(L"RuneLite not found under %LOCALAPPDATA%\\RuneLite.", COL_ERROR);
    MessageBoxW(hMain,
      L"RuneLite was not found.\r\n\r\n"
      L"Install from https://runelite.net then try again.\r\n"
      L"Expected folder: %LOCALAPPDATA%\\RuneLite\r\n"
      L"(with jre\\bin\\java.exe and RuneLite.jar)",
      L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  SetStatus(L"Starting RuneLite...", COL_PRIMARY);
  AppendResult(L"--- Open RuneLite (proxied) ---");
  std::thread([cfg]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchRuneLiteViaBridge(cfg, pid, err);
    PostRouteResult(ok, ok
      ? (L"RuneLite started (PID " + std::to_wstring(pid) + L"). Java SOCKS on shared bridge.")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoDevLaunchJagex() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (FindDefaultJagexLauncherPath().empty()) {
    SetStatus(L"Jagex Launcher not found.", COL_ERROR);
    MessageBoxW(hMain,
      L"Jagex Launcher was not found.\r\n\r\n"
      L"Expected: C:\\Program Files (x86)\\Jagex Launcher\\JagexLauncher.exe",
      L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  if (FindDefaultRuneLiteDir().empty()) {
    SetStatus(L"RuneLite not found - needed to arm SOCKS wrap for Play.", COL_ERROR);
    MessageBoxW(hMain,
      L"RuneLite was not found under %LOCALAPPDATA%\\RuneLite.\r\n\r\n"
      L"Install RuneLite so we can write SOCKS settings into config.json.\r\n"
      L"That is what makes Jagex Play -> RuneLite use the proxy.",
      L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  SetStatus(L"Starting Jagex + arming RuneLite wrap...", COL_PRIMARY);
  AppendResult(L"--- Open Jagex + arm RuneLite wrap ---");
  std::thread([cfg]() {
    DWORD pid = 0;
    int bridgePort = 0;
    std::wstring err;
    bool ok = LaunchJagexWithRuneLiteWrap(cfg, pid, bridgePort, err);
    std::wstring msg;
    if (ok) {
      msg = L"Jagex started (PID " + std::to_wstring(pid) + L"). ";
      msg += L"RuneLite wrap armed on 127.0.0.1:" + std::to_wstring(bridgePort) + L". ";
      msg += L"Hit Play in Jagex - keep this app open.";
    } else {
      msg = L"FAILED - " + err;
    }
    PostRouteResult(ok, msg);
  }).detach();
}

static void DoDevLaunchBrave() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (FindDefaultBravePath().empty()) {
    SetStatus(L"Brave not found.", COL_ERROR);
    MessageBoxW(hMain, L"brave.exe was not found. Install Brave Browser or use Browse app.",
                L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  SetStatus(L"Starting Brave...", COL_PRIMARY);
  AppendResult(L"--- Open Brave (proxied) ---");
  std::thread([cfg]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchBraveViaBridge(cfg, pid, err);
    PostRouteResult(ok, ok
      ? (L"Brave started (PID " + std::to_wstring(pid) + L"). Shared bridge.")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoDevLaunchDiscord() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (FindDefaultDiscordPath().empty()) {
    SetStatus(L"Discord not found.", COL_ERROR);
    MessageBoxW(hMain,
      L"Discord was not found under %LOCALAPPDATA%\\Discord.\r\n"
      L"Install Discord desktop, then try again.\r\n"
      L"(We keep your normal Discord profile so you stay logged in.)",
      L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  if (!ConfirmRelaunchIfRunning(L"Discord", L"Discord.exe")) return;
  SetStatus(L"Starting Discord via proxy bridge...", COL_PRIMARY);
  AppendResult(L"--- Open Discord (proxied) ---");
  std::thread([cfg]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchDiscordViaBridge(cfg, pid, err);
    PostRouteResult(ok, ok
      ? (L"Discord started (PID " + std::to_wstring(pid) +
         L"). Uses your normal profile + SOCKS bridge.")
      : (L"FAILED - " + err));
  }).detach();
}

// Generic one-shot launch helper for new easy apps
using LaunchFn = bool (*)(const ProxyConfig&, DWORD&, std::wstring&, const std::wstring&);
// Wrappers with optional path empty
static bool LaunchOp(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchOperaViaBridge(c, p, e);
}
static bool LaunchVi(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchVivaldiViaBridge(c, p, e);
}
static bool LaunchSl(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchSlackViaBridge(c, p, e);
}
static bool LaunchTm(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchTeamsViaBridge(c, p, e);
}
static bool LaunchVs(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchVSCodeViaBridge(c, p, e);
}
static bool LaunchCu(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchCursorViaBridge(c, p, e);
}
static bool LaunchPo(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchPostmanViaBridge(c, p, e);
}
static bool LaunchTb(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchThunderbirdViaBridge(c, p, e);
}
static bool LaunchSp(const ProxyConfig& c, DWORD& p, std::wstring& e, const std::wstring&) {
  return LaunchSpotifyViaBridge(c, p, e);
}

static void DoLaunchEasyApp(const wchar_t* label,
                            std::wstring (*findPath)(),
                            bool (*launch)(const ProxyConfig&, DWORD&, std::wstring&, const std::wstring&),
                            const wchar_t* imageForGuard,
                            const wchar_t* notFoundMsg) {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  if (findPath().empty()) {
    SetStatus(std::wstring(label) + L" not found.", COL_ERROR);
    MessageBoxW(hMain, notFoundMsg, L"App routing", MB_ICONWARNING | MB_OK);
    return;
  }
  if (imageForGuard && imageForGuard[0] &&
      !ConfirmRelaunchIfRunning(label, imageForGuard))
    return;
  SetStatus(std::wstring(L"Starting ") + label + L"...", COL_PRIMARY);
  AppendResult(std::wstring(L"--- Open ") + label + L" (proxied) ---");
  std::wstring name = label;
  std::thread([cfg, launch, name]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = launch(cfg, pid, err, L"");
    std::wstring msg;
    if (ok) {
      msg = name + L" started (PID " + std::to_wstring(pid) + L").";
      if (name == L"Postman") {
        msg += L" Test: GET https://api.ipify.org — must match Test Proxy IP.";
        msg += L" If not: Settings > Proxy > use system/env proxy, then restart from here.";
      } else if (name == L"VS Code" || name == L"Cursor") {
        msg += L" Uses a ProxyPi profile (settings force SOCKS). Keep extensions via default folder.";
      }
    } else {
      msg = L"FAILED - " + err;
    }
    PostRouteResult(ok, msg);
  }).detach();
}

static void DoBrowseAndLaunchApp() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;

  wchar_t file[MAX_PATH] = {};
  if (!g_customAppPath.empty() && g_customAppPath.size() < MAX_PATH)
    wcsncpy_s(file, g_customAppPath.c_str(), _TRUNCATE);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hMain;
  ofn.lpstrFilter =
    L"Chromium / Electron apps (*.exe)\0*.exe\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"Select Chromium or Electron app (Chrome, Brave, Discord, Opera…)";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) return;

  g_customAppPath = file;
  if (!IsChromiumLikeExecutable(g_customAppPath)) {
    if (MessageBoxW(hMain,
          L"This does not look like a Chromium/Electron app.\r\n"
          L"Launch with --proxy-server anyway?\r\n\r\n"
          L"(Works for many Electron apps; not for classic Win32 programs.)",
          L"Browse app", MB_YESNO | MB_ICONQUESTION) != IDYES)
      return;
  }

  // Friendly name from exe basename
  std::wstring name = g_customAppPath;
  size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos) name = name.substr(slash + 1);
  if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
    name = name.substr(0, name.size() - 4);

  // Discord-like: keep real profile if name contains Discord
  bool tempProfile = true;
  if (name.find(L"Discord") != std::wstring::npos ||
      name.find(L"discord") != std::wstring::npos)
    tempProfile = false;

  std::wstring path = g_customAppPath;
  SetStatus(L"Starting " + name + L"...", COL_PRIMARY);
  AppendResult(L"--- Open custom app (proxied) ---");
  AppendResult(L"  Path: " + path);
  std::thread([cfg, path, name, tempProfile]() {
    DWORD pid = 0;
    std::wstring err;
    std::wstring url = tempProfile ? L"http://ifconfig.me" : L"";
    bool ok = LaunchChromiumLikeViaBridge(cfg, pid, err, path, name, tempProfile, url);
    PostRouteResult(ok, ok
      ? (name + L" started (PID " + std::to_wstring(pid) + L").")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoOpenUrlViaProxy() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  wchar_t urlBuf[1024] = {};
  if (hEditOpenUrl) GetWindowTextW(hEditOpenUrl, urlBuf, 1024);
  std::wstring url = urlBuf;
  // trim
  while (!url.empty() && (url.back() == L' ' || url.back() == L'\t')) url.pop_back();
  size_t i = 0;
  while (i < url.size() && (url[i] == L' ' || url[i] == L'\t')) ++i;
  url = url.substr(i);
  if (url.empty()) {
    SetStatus(L"Enter a URL to open via the proxy.", COL_ERROR);
    MessageBoxW(hMain,
      L"Open with proxy\r\n\r\n"
      L"Paste any website (e.g. ifconfig.me or https://example.com)\r\n"
      L"and click Open URL. We launch a proxied browser to that page.\r\n\r\n"
      L"This is not a Windows right-click on links yet — it is in-app:\r\n"
      L"copy a link -> paste here -> Open URL.",
      L"Open URL", MB_OK | MB_ICONINFORMATION);
    return;
  }
  SaveAppRoutePrefs();
  SetStatus(L"Opening URL via proxied browser...", COL_PRIMARY);
  AppendResult(L"--- Open URL (proxied) ---");
  AppendResult(L"  URL: " + url);
  std::thread([cfg, url]() {
    DWORD pid = 0;
    std::wstring err;
    bool ok = LaunchUrlViaProxiedBrowser(cfg, pid, err, url);
    PostRouteResult(ok, ok
      ? (L"Browser opened URL (PID " + std::to_wstring(pid) + L").")
      : (L"FAILED - " + err));
  }).detach();
}

static void DoCopyExitIp() {
  std::wstring text;
  if (g_hasResult && !g_lastIp.empty()) {
    text = g_lastIp;
  } else {
    // Build a short session report for support
    text = L"(no exit IP yet — run Test Proxy first)";
  }
  // Also offer richer clipboard if we have more context
  std::wstring report;
  report += L"ProxyPi exit IP: " + (g_lastIp.empty() ? L"(unknown)" : g_lastIp) + L"\r\n";
  if (g_hasResult) {
    report += L"Latency: " + std::to_wstring(g_lastLatency) + L" ms\r\n";
    if (g_lastMbps >= 0) {
      wchar_t b[32];
      swprintf_s(b, L"%.2f", g_lastMbps);
      report += L"Download: ";
      report += b;
      report += L" Mbps\r\n";
    }
  }
  int bp = GetLocalAuthBridgePort();
  if (bp > 0)
    report += L"Local bridge: 127.0.0.1:" + std::to_wstring(bp) + L"\r\n";
  auto sessions = GetRoutedAppSessions();
  if (!sessions.empty()) {
    report += L"Active apps:\r\n";
    for (const auto& s : sessions) {
      report += L"  - " + s.name + L" PID=" + std::to_wstring(s.pid) +
                L" conns=" + std::to_wstring(s.connCount) + L"\r\n";
    }
  }

  if (!OpenClipboard(hMain)) {
    SetStatus(L"Could not open clipboard.", COL_ERROR);
    return;
  }
  EmptyClipboard();
  // Prefer full report for support; pure IP still first line
  size_t bytes = (report.size() + 1) * sizeof(wchar_t);
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (hMem) {
    void* p = GlobalLock(hMem);
    if (p) {
      memcpy(p, report.c_str(), bytes);
      GlobalUnlock(hMem);
      SetClipboardData(CF_UNICODETEXT, hMem);
    }
  }
  CloseClipboard();

  if (g_hasResult && !g_lastIp.empty()) {
    SetStatus(L"Copied exit IP " + g_lastIp + L" (+ session notes) to clipboard.", COL_SUCCESS);
    AppendResult(L"Copied to clipboard: " + g_lastIp);
  } else {
    SetStatus(L"Copied session notes (run Test Proxy for exit IP).", COL_TEXT_DIM);
    AppendResult(L"Copied session report (no exit IP yet).");
  }
}

static void DoLaunchSelectedApp() {
  if (!hComboApps) return;
  int sel = (int)SendMessageW(hComboApps, CB_GETCURSEL, 0, 0);
  g_lastAppSel = sel;
  SaveAppRoutePrefs();
  // Order must match CB_ADDSTRING list in WM_CREATE
  switch (sel) {
    case 0: DoDevLaunchChrome(); break;
    case 1: DoDevLaunchEdge(); break;
    case 2: DoDevLaunchBrave(); break;
    case 3: DoDevLaunchFirefox(); break;
    case 4: DoDevLaunchDiscord(); break;
    case 5:
      DoLaunchEasyApp(L"Opera", FindDefaultOperaPath, LaunchOp, L"opera.exe",
        L"Opera was not found. Install Opera or use Browse.");
      break;
    case 6:
      DoLaunchEasyApp(L"Vivaldi", FindDefaultVivaldiPath, LaunchVi, L"vivaldi.exe",
        L"Vivaldi was not found. Install Vivaldi or use Browse.");
      break;
    case 7:
      DoLaunchEasyApp(L"Slack", FindDefaultSlackPath, LaunchSl, L"slack.exe",
        L"Slack was not found under %LOCALAPPDATA%\\slack.");
      break;
    case 8:
      DoLaunchEasyApp(L"Teams", FindDefaultTeamsPath, LaunchTm, L"Teams.exe",
        L"Microsoft Teams was not found.");
      break;
    case 9:
      DoLaunchEasyApp(L"VS Code", FindDefaultVSCodePath, LaunchVs, L"Code.exe",
        L"VS Code was not found (Code.exe).");
      break;
    case 10:
      DoLaunchEasyApp(L"Cursor", FindDefaultCursorPath, LaunchCu, L"Cursor.exe",
        L"Cursor was not found.");
      break;
    case 11:
      DoLaunchEasyApp(L"Postman", FindDefaultPostmanPath, LaunchPo, L"Postman.exe",
        L"Postman was not found under %LOCALAPPDATA%\\Postman.");
      break;
    case 12:
      DoLaunchEasyApp(L"Thunderbird", FindDefaultThunderbirdPath, LaunchTb, L"thunderbird.exe",
        L"Thunderbird was not found.");
      break;
    case 13:
      DoLaunchEasyApp(L"Spotify", FindDefaultSpotifyPath, LaunchSp, L"Spotify.exe",
        L"Spotify was not found.\r\nNote: Microsoft Store builds may ignore proxy flags.");
      break;
    case 14: DoDevLaunchRuneLite(); break;
    case 15: DoDevLaunchJagex(); break;
    case 16: DoBrowseAndLaunchApp(); break;
    default:
      SetStatus(L"Select an app from the dropdown first.", COL_ERROR);
      break;
  }
}

static void DoCheckUpdates(bool silent) {
  if (!silent) {
    SetStatus(L"Checking for updates...", COL_PRIMARY);
    AppendResult(L"--- Update check ---");
  }
  std::thread([silent]() {
    UpdateCheckResult r = PerformUpdateCheck();
    r.silent = silent;
    auto* heap = new UpdateCheckResult(std::move(r));
    PostMessageW(hMain, WM_APP_UPDATE_DONE, 0, (LPARAM)heap);
  }).detach();
}

static void DoLoginLoadProxy() {
  if (g_busy) return;
  wchar_t emailBuf[512] = {}, passBuf[512] = {};
  GetWindowTextW(hAcctEmail, emailBuf, 512);
  GetWindowTextW(hAcctPass, passBuf, 512);
  std::wstring email = emailBuf;
  std::wstring pass = passBuf;
  if (email.empty() || pass.empty()) {
    SetStatus(L"Enter your ProxyPi account email and password.", COL_ERROR);
    return;
  }
  SetBusy(true);
  if (hBtnLogin) EnableWindow(hBtnLogin, FALSE);
  SetStatus(L"Logging in and loading your assigned proxy...", COL_PRIMARY);
  AppendResult(L"--- Account login ---");

  std::thread([email, pass]() {
    DesktopLoginResult r = PerformDesktopLogin(email, pass);
    auto* heap = new DesktopLoginResult(std::move(r));
    PostMessageW(hMain, WM_APP_LOGIN_DONE, 0, (LPARAM)heap);
  }).detach();
}

// ---- custom button (owner-draw primary / secondary) ----
struct BtnStyle {
  bool primary;
  bool hover;
  bool pressed;
};

static BtnStyle GetBtnStyle(HWND hwnd) {
  BtnStyle s{};
  s.primary = (GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0);
  POINT pt; GetCursorPos(&pt);
  RECT rc; GetWindowRect(hwnd, &rc);
  s.hover = PtInRect(&rc, pt) != 0;
  s.pressed = (GetCapture() == hwnd) && s.hover;
  return s;
}

static LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc; GetClientRect(hwnd, &rc);
      BtnStyle st = GetBtnStyle(hwnd);

      COLORREF fill, border, text;
      if (st.primary) {
        fill = st.pressed ? COL_PRIMARY_DD : (st.hover ? COL_PRIMARY_D : COL_PRIMARY);
        border = fill;
        text = RGB(255, 255, 255);
      } else {
        fill = st.pressed ? RGB(30, 41, 59) : (st.hover ? RGB(30, 41, 59) : COL_CARD);
        border = st.hover ? COL_PRIMARY : COL_BORDER;
        text = COL_TEXT;
      }

      HBRUSH br = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, border);
      HGDIOBJ oldBr = SelectObject(hdc, br);
      HGDIOBJ oldPen = SelectObject(hdc, pen);
      RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
      SelectObject(hdc, oldBr);
      SelectObject(hdc, oldPen);
      DeleteObject(br);
      DeleteObject(pen);

      wchar_t caption[128];
      GetWindowTextW(hwnd, caption, 128);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, text);
      SelectObject(hdc, hFontUi);
      DrawTextW(hdc, caption, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
      TrackMouseEvent(&tme);
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    }
    case WM_MOUSELEAVE:
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_LBUTTONUP:
      if (GetCapture() == hwnd) {
        ReleaseCapture();
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc; GetClientRect(hwnd, &rc);
        if (PtInRect(&rc, pt)) {
          SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        }
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static HWND CreateStyledButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                               int id, bool primary) {
  HWND btn = CreateWindowW(L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
    x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
  SetWindowLongPtrW(btn, GWLP_USERDATA, primary ? 1 : 0);
  SetWindowSubclass(btn, BtnProc, 1, 0);
  SendMessageW(btn, WM_SETFONT, (WPARAM)hFontUi, TRUE);
  return btn;
}

// Dark edit background via CTLCOLOR
static HBRUSH OnCtlColorEdit(HDC hdc) {
  SetTextColor(hdc, COL_TEXT);
  SetBkColor(hdc, COL_BG_DEEP);
  return hBrushInput;
}

static HBRUSH OnCtlColorStatic(HDC hdc, HWND hwnd) {
  if (hwnd == hStatus) {
    SetTextColor(hdc, g_statusColor);
  } else {
    SetTextColor(hdc, COL_TEXT_DIM);
  }
  SetBkColor(hdc, COL_BG);
  return hBrushBg;
}

// ---- actions ----
static void DoTest() {
  if (g_busy) return;
  ProxyConfig cfg = ReadConfigFromUI();
  if (cfg.host.empty()) {
    SetStatus(L"Please enter a proxy host.", COL_ERROR);
    return;
  }
  SetBusy(true);
  SetStatus(L"Testing proxy connectivity...", COL_PRIMARY);
  AppendResult(L"--- Connectivity test ---");

  std::thread([cfg]() {
    ProxyTestResult r = TestProxy(cfg, "api.ipify.org", 80);
    auto* heap = new ProxyTestResult(r);
    PostMessageW(hMain, WM_APP_TEST_DONE, 0, (LPARAM)heap);
  }).detach();
}

static void DoSpeed() {
  if (g_busy) return;
  ProxyConfig cfg = ReadConfigFromUI();
  if (cfg.host.empty()) {
    SetStatus(L"Please enter a proxy host.", COL_ERROR);
    return;
  }
  SetBusy(true);
  SetStatus(L"Running speed test (IP check + ~1 MB download)...", COL_PRIMARY);
  AppendResult(L"--- Speed test ---");

  std::thread([cfg]() {
    SpeedTestResult r = SpeedTestProxy(cfg);
    auto* heap = new SpeedTestResult(r);
    PostMessageW(hMain, WM_APP_SPEED_DONE, 0, (LPARAM)heap);
  }).detach();
}

// ---- painting ----
static void FillRoundRect(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border) {
  HBRUSH br = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldBr = SelectObject(hdc, br);
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
  SelectObject(hdc, oldBr);
  SelectObject(hdc, oldPen);
  DeleteObject(br);
  DeleteObject(pen);
}

static void DrawTextAt(HDC hdc, const wchar_t* text, int x, int y, int w, COLORREF color, HFONT font, UINT flags = DT_LEFT) {
  RECT rc{ x, y, x + w, y + 40 };
  SelectObject(hdc, font);
  SetTextColor(hdc, color);
  SetBkMode(hdc, TRANSPARENT);
  DrawTextW(hdc, text, -1, &rc, flags | DT_SINGLELINE | DT_NOPREFIX);
}

static void PaintHeader(HDC hdc, int width) {
  // Dark gradient-ish header band
  RECT top{ 0, 0, width, 110 };
  HBRUSH br = CreateSolidBrush(COL_BG);
  FillRect(hdc, &top, br);
  DeleteObject(br);

  // Accent bar
  RECT bar{ 0, 0, width, 3 };
  HBRUSH brBar = CreateSolidBrush(COL_PRIMARY);
  FillRect(hdc, &bar, brBar);
  DeleteObject(brBar);

  // Soft glow ellipse (approximation with solid alpha-ish circle via layered fill)
  // Title
  DrawTextAt(hdc, L"ProxyPiTester", PAD, 22, 360, COL_PRIMARY, hFontTitle);
  DrawTextAt(hdc, L"UK residential proxy test & app routing", PAD, 58, 420, COL_TEXT_DIM, hFontSmall);
  DrawTextAt(hdc, L"proxypi.co.uk", width - PAD - 140, 28, 140, COL_TEXT_DIM, hFontSmall, DT_RIGHT);
}

static void PaintMetricCards(HDC hdc, int y) {
  int cardH = 72;
  int gap = 10;
  int w3 = (g_lay.cardW - gap * 2) / 3;

  auto paintOne = [&](int x, const wchar_t* label, const std::wstring& value, COLORREF valueColor) {
    RECT rc{ x, y, x + w3, y + cardH };
    FillRoundRect(hdc, rc, 12, COL_CARD, COL_BORDER);
    DrawTextAt(hdc, label, x + 14, y + 10, w3 - 28, COL_TEXT_DIM, hFontSmall);
    DrawTextAt(hdc, value.c_str(), x + 14, y + 32, w3 - 28, valueColor, hFontUi);
  };

  std::wstring lat = g_hasResult ? (std::to_wstring(g_lastLatency) + L" ms") : L"-";
  std::wstring ip = g_hasResult && !g_lastIp.empty() ? g_lastIp : L"-";
  std::wstring spd = L"-";
  if (g_hasResult && g_lastMbps >= 0.0) {
    wchar_t b[32];
    swprintf_s(b, L"%.2f Mbps", g_lastMbps);
    spd = b;
  }

  COLORREF okCol = g_hasResult ? (g_lastOk ? COL_SUCCESS : COL_ERROR) : COL_TEXT_DIM;
  paintOne(g_lay.cardX, L"LATENCY", lat, okCol);
  paintOne(g_lay.cardX + w3 + gap, L"EXIT IP", ip, g_hasResult && g_lastOk ? COL_PRIMARY : COL_TEXT_DIM);
  paintOne(g_lay.cardX + (w3 + gap) * 2, L"DOWNLOAD", spd, g_hasResult && g_lastMbps >= 0 ? COL_PRIMARY : COL_TEXT_DIM);
}

static void PaintCardChrome(HDC hdc, RECT rc, const wchar_t* title) {
  FillRoundRect(hdc, rc, 14, COL_CARD, COL_BORDER);
  DrawTextAt(hdc, title, rc.left + 16, rc.top + 12, rc.right - rc.left - 32, COL_PRIMARY, hFontSmall);
}

static void ComputeLayout(int cw, int ch) {
  if (cw < MIN_CLIENT_W) cw = MIN_CLIENT_W;
  if (ch < MIN_CLIENT_H) ch = MIN_CLIENT_H;
  g_lay.clientW = cw;
  g_lay.clientH = ch;
  g_lay.cardX = 20;
  g_lay.cardW = cw - 40;
  g_lay.sideBySide = (cw >= SIDE_BY_SIDE_MIN_W);

  const int headerEnd = 108;
  const int gap = 12;
  const int metricsH = 78;
  const int actionsH = 86; // fixed-size buttons, not stretched
  // Compact app toolbar (dropdown + URL row) — frees height for session list
  const int routeChrome = 118; // title + bridge + app row + URL row
  const int resMinH = 90;
  const int footerBand = 96; // status strip + clean footer line

  if (g_lay.sideBySide) {
    // Account | Config side by side under header
    int half = (g_lay.cardW - gap) / 2;
    g_lay.acctLeft = g_lay.cardX;
    g_lay.acctW = half;
    g_lay.cfgLeft = g_lay.cardX + half + gap;
    g_lay.cfgW = half;
    g_lay.acctTop = headerEnd + 4;
    g_lay.cfgTop = g_lay.acctTop;
    // Config has more rows; shared card height
    g_lay.acctBot = g_lay.acctTop + 210;
    g_lay.cfgBot = g_lay.acctBot;
    g_lay.metricsY = g_lay.acctBot + gap;
  } else {
    g_lay.acctLeft = g_lay.cardX;
    g_lay.acctW = g_lay.cardW;
    g_lay.cfgLeft = g_lay.cardX;
    g_lay.cfgW = g_lay.cardW;
    g_lay.acctTop = headerEnd + 4;
    g_lay.acctBot = g_lay.acctTop + 138;
    g_lay.cfgTop = g_lay.acctBot + gap;
    g_lay.cfgBot = g_lay.cfgTop + 208;
    g_lay.metricsY = g_lay.cfgBot + gap;
  }

  g_lay.testBtnY = g_lay.metricsY + metricsH + 10;
  g_lay.secBtnY = g_lay.testBtnY + 46;

  // Everything below actions is flex: huge sessions + modest results
  int flexTop = g_lay.secBtnY + 42;
  int flexBot = ch - footerBand;
  if (flexBot < flexTop + 220) flexBot = flexTop + 220;
  int flexH = flexBot - flexTop;

  // Give most space to multi-app sessions
  int routeH = (int)(flexH * 0.72);
  if (routeH < routeChrome + 120) routeH = routeChrome + 120;
  int resH = flexH - routeH - gap;
  if (resH < resMinH) {
    resH = resMinH;
    routeH = flexH - resH - gap;
  }

  g_lay.routeTop = flexTop;
  g_lay.routeBot = flexTop + routeH;
  g_lay.resTop = g_lay.routeBot + gap;
  g_lay.resBot = flexBot;
  g_lay.statusY = ch - 84;  // status card
  g_lay.footerY = ch - 22;  // branding line under status (room for 16px font)
}

static void MoveCtrl(HWND h, int x, int y, int w, int hh) {
  if (h && IsWindow(h))
    SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void HideCtrl(HWND h) {
  if (h && IsWindow(h)) ShowWindow(h, SW_HIDE);
}

static void ApplyLayout(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  ComputeLayout(rc.right - rc.left, rc.bottom - rc.top);

  const int rowH = 30;
  const int gapY = 32;
  const int labelW = 78;

  auto layoutFields = [&](int left, int top, int cardW, bool account) {
    int pad = 14;
    int labelX = left + pad;
    int fieldX = left + pad + labelW;
    int fieldW = cardW - pad * 2 - labelW;
    if (fieldW < 100) fieldW = 100;
    int y = top + 34;
    if (account) {
      MoveCtrl(hLblAcctEmail, labelX, y + 4, labelW, 18);
      MoveCtrl(hAcctEmail, fieldX, y, fieldW, rowH);
      y += gapY;
      MoveCtrl(hLblAcctPass, labelX, y + 4, labelW, 18);
      MoveCtrl(hAcctPass, fieldX, y, fieldW, rowH);
      y += gapY + 2;
      // Fixed-width login button (does not stretch full card)
      int loginW = BTN_LOGIN_W;
      if (loginW > cardW - pad * 2) loginW = cardW - pad * 2;
      MoveCtrl(hBtnLogin, left + pad, y, loginW, 34);
    } else {
      MoveCtrl(hLblType, labelX, y + 4, labelW, 18);
      int typeW = fieldW > 160 ? 160 : fieldW;
      MoveCtrl(hComboType, fieldX, y, typeW, 200);
      y += gapY;
      MoveCtrl(hLblHost, labelX, y + 4, labelW, 18);
      MoveCtrl(hHost, fieldX, y, fieldW, rowH);
      y += gapY;
      MoveCtrl(hLblPort, labelX, y + 4, labelW, 18);
      MoveCtrl(hPort, fieldX, y, 90, rowH);
      int authX = fieldX + 100;
      int authW = left + cardW - pad - authX;
      if (authW < 120) authW = 120;
      MoveCtrl(hCheckAuth, authX, y + 3, authW, 24);
      y += gapY;
      MoveCtrl(hLblUser, labelX, y + 4, labelW, 18);
      MoveCtrl(hUser, fieldX, y, fieldW, rowH);
      y += gapY;
      MoveCtrl(hLblPass, labelX, y + 4, labelW, 18);
      MoveCtrl(hPass, fieldX, y, fieldW, rowH);
    }
  };

  layoutFields(g_lay.acctLeft, g_lay.acctTop, g_lay.acctW, true);
  layoutFields(g_lay.cfgLeft, g_lay.cfgTop, g_lay.cfgW, false);

  // Fixed-size action buttons (left-aligned, never stretch)
  const int cx = g_lay.cardX;
  int x = cx;
  MoveCtrl(hBtnTest, x, g_lay.testBtnY, BTN_PRIMARY_W, 40);
  x += BTN_PRIMARY_W + 10;
  MoveCtrl(hBtnSpeed, x, g_lay.testBtnY, BTN_PRIMARY_W, 40);
  x = cx;
  MoveCtrl(hBtnSave, x, g_lay.secBtnY, BTN_SECONDARY_W, 32);
  x += BTN_SECONDARY_W + 8;
  MoveCtrl(hBtnLoad, x, g_lay.secBtnY, BTN_SECONDARY_W, 32);
  x += BTN_SECONDARY_W + 8;
  MoveCtrl(hBtnSite, x, g_lay.secBtnY, BTN_SECONDARY_W, 32);

  // Hide per-app grid buttons; use dropdown instead
  HideCtrl(hBtnDevChrome);
  HideCtrl(hBtnDevEdge);
  HideCtrl(hBtnDevFirefox);
  HideCtrl(hBtnDevRuneLite);
  HideCtrl(hBtnDevJagex);

  // App routing toolbar
  int rx = g_lay.cardX + 14;
  int rw = g_lay.cardW - 28;
  MoveCtrl(hChromePathLabel, rx, g_lay.routeTop + 28, rw, 18);

  int toolY = g_lay.routeTop + 50;
  x = rx;
  MoveCtrl(hComboApps, x, toolY, COMBO_APPS_W, 200);
  x += COMBO_APPS_W + 8;
  MoveCtrl(hBtnLaunchApp, x, toolY - 1, BTN_LAUNCH_W, 32);
  x += BTN_LAUNCH_W + 8;
  MoveCtrl(hBtnBrowseApp, x, toolY - 1, BTN_REFRESH_W, 32);
  x += BTN_REFRESH_W + 8;
  MoveCtrl(hBtnSessRefresh, x, toolY - 1, BTN_REFRESH_W, 32);

  // URL row: open link via proxied browser + copy exit IP
  int urlY = g_lay.routeTop + 88;
  int urlEditW = rw - BTN_URL_W - BTN_COPY_W - 16;
  if (urlEditW < 120) urlEditW = 120;
  MoveCtrl(hEditOpenUrl, rx, urlY, urlEditW, 28);
  MoveCtrl(hBtnOpenUrl, rx + urlEditW + 8, urlY - 2, BTN_URL_W, 32);
  MoveCtrl(hBtnCopyIp, rx + urlEditW + 8 + BTN_URL_W + 8, urlY - 2, BTN_COPY_W, 32);

  // Large session list (main multi-app view)
  int sessTop = g_lay.routeTop + 126;
  int sessH = g_lay.routeBot - sessTop - 10;
  if (sessH < 80) sessH = 80;
  MoveCtrl(hSessionList, rx, sessTop, rw, sessH);

  // Results (compact but usable)
  int resInnerTop = g_lay.resTop + 26;
  int resInnerH = g_lay.resBot - resInnerTop - 8;
  if (resInnerH < 48) resInnerH = 48;
  MoveCtrl(hResult, rx, resInnerTop, rw, resInnerH);

  MoveCtrl(hStatus, g_lay.cardX + 8, g_lay.statusY + 6, g_lay.cardW - 16, 28);

  InvalidateRect(hwnd, nullptr, FALSE);
}

// ---- window proc ----
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE: {
      hMain = hwnd;

      // Fonts — Inter if installed, else Segoe UI
      hFontTitle = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontUi = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontSmall = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontMono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");

      hBrushBg = CreateSolidBrush(COL_BG);
      hBrushCard = CreateSolidBrush(COL_CARD);
      hBrushInput = CreateSolidBrush(COL_BG_DEEP);
      hBrushPrimary = CreateSolidBrush(COL_PRIMARY);

      // Controls created at placeholder coords; ApplyLayout positions everything.
      auto mkLabel = [&](const wchar_t* t) {
        HWND s = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE,
          0, 0, 90, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(s, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        return s;
      };

      hLblAcctEmail = mkLabel(L"Email");
      hAcctEmail = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_ACCT_EMAIL, nullptr, nullptr);
      SendMessageW(hAcctEmail, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hLblAcctPass = mkLabel(L"Password");
      hAcctPass = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_ACCT_PASS, nullptr, nullptr);
      SendMessageW(hAcctPass, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      // Note: do not use single '&' in labels — Win32 treats it as accelerator
      hBtnLogin = CreateStyledButton(hwnd, L"Login and Load My Proxy",
        0, 0, 100, 36, ID_BTN_LOGIN, true);

      hLblType = mkLabel(L"Type");
      hComboType = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 160, 200, hwnd, (HMENU)ID_COMBO_TYPE, nullptr, nullptr);
      SendMessageW(hComboType, CB_ADDSTRING, 0, (LPARAM)L"SOCKS5");
      SendMessageW(hComboType, CB_ADDSTRING, 0, (LPARAM)L"HTTP / HTTPS");
      SendMessageW(hComboType, CB_SETCURSEL, 0, 0);
      SendMessageW(hComboType, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hLblHost = mkLabel(L"Host");
      hHost = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_HOST, nullptr, nullptr);
      SendMessageW(hHost, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hLblPort = mkLabel(L"Port");
      hPort = CreateWindowW(L"EDIT", L"18721",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_PORT, nullptr, nullptr);
      SendMessageW(hPort, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hCheckAuth = CreateWindowW(L"BUTTON", L"  Authentication",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 160, 24, hwnd, (HMENU)ID_CHECK_AUTH, nullptr, nullptr);
      SendMessageW(hCheckAuth, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
      SendMessageW(hCheckAuth, BM_SETCHECK, BST_CHECKED, 0);

      hLblUser = mkLabel(L"Username");
      hUser = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_USER, nullptr, nullptr);
      SendMessageW(hUser, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hLblPass = mkLabel(L"Password");
      hPass = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_PASS, nullptr, nullptr);
      SendMessageW(hPass, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hBtnTest = CreateStyledButton(hwnd, L"Test Proxy", 0, 0, 100, 42, ID_BTN_TEST, true);
      hBtnSpeed = CreateStyledButton(hwnd, L"Speed Test", 0, 0, 100, 42, ID_BTN_SPEED, true);
      hBtnSave = CreateStyledButton(hwnd, L"Save", 0, 0, 80, 34, ID_BTN_SAVE, false);
      hBtnLoad = CreateStyledButton(hwnd, L"Reload", 0, 0, 80, 34, ID_BTN_LOAD, false);
      hBtnSite = CreateStyledButton(hwnd, L"Website", 0, 0, 80, 34, ID_BTN_SITE, false);

      hChromePathLabel = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
        0, 0, 100, 18, hwnd, (HMENU)ID_STATIC_DEV_PATH, nullptr, nullptr);
      SendMessageW(hChromePathLabel, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

      // Apps dropdown (replaces grid of launch buttons)
      hComboApps = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, COMBO_APPS_W, 200, hwnd, (HMENU)ID_COMBO_APPS, nullptr, nullptr);
      SendMessageW(hComboApps, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Chrome");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Edge");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Brave");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Firefox");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Discord");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Opera");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Vivaldi");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Slack");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Teams");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"VS Code");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Cursor");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Postman");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Thunderbird");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Spotify");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"RuneLite");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Jagex (+ RuneLite wrap)");
      SendMessageW(hComboApps, CB_ADDSTRING, 0, (LPARAM)L"Browse Chromium app...");
      SendMessageW(hComboApps, CB_SETCURSEL, 0, 0);

      hBtnLaunchApp = CreateStyledButton(hwnd, L"Open app", 0, 0, BTN_LAUNCH_W, 32, ID_BTN_LAUNCH_APP, true);
      hBtnBrowseApp = CreateStyledButton(hwnd, L"Browse", 0, 0, BTN_REFRESH_W, 32, ID_BTN_BROWSE_APP, false);
      hBtnSessRefresh = CreateStyledButton(hwnd, L"Refresh", 0, 0, BTN_REFRESH_W, 32, ID_BTN_SESS_REFRESH, false);

      hEditOpenUrl = CreateWindowW(L"EDIT", L"https://ifconfig.me",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 200, 28, hwnd, (HMENU)ID_EDIT_OPEN_URL, nullptr, nullptr);
      SendMessageW(hEditOpenUrl, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      hBtnOpenUrl = CreateStyledButton(hwnd, L"Open URL", 0, 0, BTN_URL_W, 32, ID_BTN_OPEN_URL, true);
      hBtnCopyIp = CreateStyledButton(hwnd, L"Copy IP", 0, 0, BTN_COPY_W, 32, ID_BTN_COPY_IP, false);

      // Legacy grid buttons kept hidden for menu-id compatibility (not shown)
      hBtnDevChrome = CreateStyledButton(hwnd, L"Chrome", 0, 0, 1, 1, ID_BTN_DEV_CHROME, true);
      hBtnDevEdge = CreateStyledButton(hwnd, L"Edge", 0, 0, 1, 1, ID_BTN_DEV_EDGE, true);
      hBtnDevFirefox = CreateStyledButton(hwnd, L"Firefox", 0, 0, 1, 1, ID_BTN_DEV_FIREFOX, true);
      hBtnDevRuneLite = CreateStyledButton(hwnd, L"RuneLite", 0, 0, 1, 1, ID_BTN_DEV_RUNELITE, true);
      hBtnDevJagex = CreateStyledButton(hwnd, L"Jagex", 0, 0, 1, 1, ID_BTN_DEV_JAGEX, true);
      HideCtrl(hBtnDevChrome); HideCtrl(hBtnDevEdge); HideCtrl(hBtnDevFirefox);
      HideCtrl(hBtnDevRuneLite); HideCtrl(hBtnDevJagex);
      hBtnBrowseChrome = nullptr;

      hSessionList = CreateWindowW(L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
        0, 0, 100, 80, hwnd, (HMENU)ID_LIST_SESSIONS, nullptr, nullptr);
      SendMessageW(hSessionList, WM_SETFONT, (WPARAM)hFontMono, TRUE);

      hResult = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 100, 60, hwnd, (HMENU)ID_EDIT_RESULT, nullptr, nullptr);
      SendMessageW(hResult, WM_SETFONT, (WPARAM)hFontMono, TRUE);

      hStatus = CreateWindowW(L"STATIC", g_statusText.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
        0, 0, 100, 28, hwnd, (HMENU)ID_STATIC_STATUS, nullptr, nullptr);
      SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

      ApplyLayout(hwnd);

      // Defaults / load
      ProxyConfig cfg;
      {
        wchar_t chromeBuf[MAX_PATH] = {};
        GetPrivateProfileStringW(L"AppRoute", L"ChromePath", L"", chromeBuf, MAX_PATH, GetIniPath().c_str());
        g_chromePath = chromeBuf;
      }
      if (LoadIni(cfg)) {
        WriteConfigToUI(cfg);
        SetStatus(L"Loaded saved settings.", COL_TEXT_DIM);
      } else {
        cfg.type = ProxyType::SOCKS5;
        cfg.host = L"";
        cfg.port = 18721;
        cfg.useAuth = true;
        WriteConfigToUI(cfg);
        SetStatus(L"Ready - enter your ProxyPi host, port and credentials.", COL_TEXT_DIM);
      }
      LoadAppRoutePrefs();
      RefreshChromePathLabel();
      SetLogWindow(hResult);
      SetLogMainWindow(hwnd);

      AppendResult(L"ProxyPiTester ready.");
      AppendResult(L"Log in to load your proxy, then Test or open apps through the bridge.");
      AppendResult(L"Tip: SOCKS5 + auth recommended. First latency test is often slower.");
      RefreshSessionList();
      SetTimer(hwnd, TIMER_SESSIONS, 2000, nullptr);
      DoCheckUpdates(true);
      if (hAcctEmail) SetFocus(hAcctEmail);
      return 0;
    }

    case WM_TIMER:
      if (wParam == TIMER_SESSIONS) {
        RefreshSessionList();
        RefreshChromePathLabel();
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);

      // Double buffer
      RECT client; GetClientRect(hwnd, &client);
      HDC mem = CreateCompatibleDC(hdc);
      HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);

      HBRUSH bg = CreateSolidBrush(COL_BG);
      FillRect(mem, &client, bg);
      DeleteObject(bg);

      PaintHeader(mem, client.right);

      // Keep paint metrics in sync with current client size
      ComputeLayout(client.right, client.bottom);

      RECT acctCard{ g_lay.acctLeft, g_lay.acctTop, g_lay.acctLeft + g_lay.acctW, g_lay.acctBot };
      PaintCardChrome(mem, acctCard, L"PROXYPI ACCOUNT");

      RECT cfgCard{ g_lay.cfgLeft, g_lay.cfgTop, g_lay.cfgLeft + g_lay.cfgW, g_lay.cfgBot };
      PaintCardChrome(mem, cfgCard, L"PROXY CONFIGURATION");

      PaintMetricCards(mem, g_lay.metricsY);

      RECT routeCard{ g_lay.cardX, g_lay.routeTop, g_lay.cardX + g_lay.cardW, g_lay.routeBot };
      PaintCardChrome(mem, routeCard, L"ACTIVE APPS");

      RECT resCard{ g_lay.cardX, g_lay.resTop, g_lay.cardX + g_lay.cardW, g_lay.resBot };
      PaintCardChrome(mem, resCard, L"RESULTS");

      RECT statusBg{ g_lay.cardX, g_lay.statusY, g_lay.cardX + g_lay.cardW, g_lay.statusY + 40 };
      FillRoundRect(mem, statusBg, 10, COL_CARD, COL_BORDER);

      // Footer branding — ASCII only (no fancy dots that mojibake), full client width
      {
        std::wstring foot = L"No logging | UK residential | Pay as you go | v";
        foot += APP_VERSION;
        RECT footRc{ 8, g_lay.footerY - 2, client.right - 8, g_lay.footerY + 18 };
        SelectObject(mem, hFontSmall);
        SetTextColor(mem, COL_TEXT_DIM);
        SetBkMode(mem, TRANSPARENT);
        DrawTextW(mem, foot.c_str(), -1, &footRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      }

      BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return (LRESULT)OnCtlColorEdit((HDC)wParam);

    case WM_CTLCOLORSTATIC:
      return (LRESULT)OnCtlColorStatic((HDC)wParam, (HWND)lParam);

    case WM_CTLCOLORBTN:
      SetBkMode((HDC)wParam, TRANSPARENT);
      return (LRESULT)hBrushBg;

    case WM_DRAWITEM: {
      // Owner-draw handled by subclass paint; ignore default
      return TRUE;
    }

    case WM_COMMAND: {
      switch (LOWORD(wParam)) {
        case ID_BTN_LOGIN:
          DoLoginLoadProxy();
          break;
        case ID_BTN_LAUNCH_APP:
          DoLaunchSelectedApp();
          break;
        case ID_BTN_BROWSE_APP:
          DoBrowseAndLaunchApp();
          break;
        case ID_BTN_OPEN_URL:
          DoOpenUrlViaProxy();
          break;
        case ID_BTN_COPY_IP:
          DoCopyExitIp();
          break;
        case ID_BTN_DEV_CHROME:
        case IDM_HELP_OPEN_CHROME:
          DoDevLaunchChrome();
          break;
        case ID_BTN_DEV_EDGE:
        case IDM_HELP_OPEN_EDGE:
          DoDevLaunchEdge();
          break;
        case ID_BTN_DEV_FIREFOX:
        case IDM_HELP_OPEN_FIREFOX:
          DoDevLaunchFirefox();
          break;
        case ID_BTN_DEV_RUNELITE:
        case IDM_HELP_OPEN_RUNELITE:
          DoDevLaunchRuneLite();
          break;
        case ID_BTN_DEV_JAGEX:
        case IDM_HELP_OPEN_JAGEX:
          DoDevLaunchJagex();
          break;
        case ID_BTN_SESS_REFRESH:
          RefreshSessionList();
          RefreshChromePathLabel();
          SetStatus(L"Session list refreshed.", COL_TEXT_DIM);
          break;
        case ID_BTN_BROWSE_CHROME:
        case IDM_HELP_SET_CHROME:
          BrowseForChromePath(hwnd);
          break;
        case IDM_HELP_CLEAR_CHROME:
          ClearChromePath();
          break;
        case IDM_HELP_APPROUTE:
          ShowAppRouteHelp(hwnd);
          break;
        case ID_COMBO_TYPE: {
          // Handle all selection notifications so port swaps reliably
          const WORD note = HIWORD(wParam);
          if (note == CBN_SELCHANGE || note == CBN_SELENDOK || note == CBN_CLOSEUP || note == CBN_SELENDCANCEL) {
            ApplyPortForSelectedType();
          }
          break;
        }
        case ID_BTN_TEST:
          DoTest();
          break;
        case ID_BTN_SPEED:
          DoSpeed();
          break;
        case ID_BTN_SAVE: {
          SaveIni(ReadConfigFromUI());
          SetStatus(L"Settings saved.", COL_SUCCESS);
          AppendResult(L"Settings saved to ProxyPiTester.ini");
          break;
        }
        case ID_BTN_LOAD: {
          ProxyConfig c;
          if (LoadIni(c)) {
            WriteConfigToUI(c);
            SetStatus(L"Settings reloaded.", COL_SUCCESS);
          } else {
            SetStatus(L"No saved settings found.", COL_TEXT_DIM);
          }
          break;
        }
        case ID_BTN_SITE:
        case IDM_HELP_WEBSITE:
          ShellExecuteW(hwnd, L"open", WEBSITE_URL, nullptr, nullptr, SW_SHOWNORMAL);
          break;
        case ID_CHECK_AUTH: {
          BOOL en = SendMessageW(hCheckAuth, BM_GETCHECK, 0, 0) == BST_CHECKED;
          EnableWindow(hUser, en);
          EnableWindow(hPass, en);
          break;
        }
        case IDM_FILE_EXIT:
          DestroyWindow(hwnd);
          break;
        case IDM_HELP_UPDATE:
          DoCheckUpdates(false);
          break;
        case IDM_HELP_ABOUT:
          ShowAboutDialog(hwnd);
          break;
      }
      return 0;
    }

    case WM_APP_ROUTE_DONE: { // App launch finished (multi-app; does not lock UI)
      std::wstring* msg = reinterpret_cast<std::wstring*>(lParam);
      if (msg) {
        AppendResult(*msg);
        SetStatus(*msg, wParam ? COL_SUCCESS : COL_ERROR);
        delete msg;
      }
      RefreshChromePathLabel();
      RefreshSessionList();
      return 0;
    }

    case WM_APP_LOG: { // thread-safe Log() from bridge workers
      std::wstring* line = reinterpret_cast<std::wstring*>(lParam);
      if (line) {
        // line already has timestamp + \r\n from Log()
        if (hResult && IsWindow(hResult)) {
          int len = GetWindowTextLengthW(hResult);
          SendMessageW(hResult, EM_SETSEL, (WPARAM)len, (LPARAM)len);
          SendMessageW(hResult, EM_REPLACESEL, FALSE, (LPARAM)line->c_str());
        }
        delete line;
      }
      return 0;
    }

    case WM_APP_TEST_DONE: {
      ProxyTestResult* r = reinterpret_cast<ProxyTestResult*>(lParam);
      SetBusy(false);
      if (r) {
        g_hasResult = true;
        g_lastOk = r->success;
        g_lastIp = r->detectedIp;
        g_lastLatency = r->latencyMs;
        g_lastMbps = -1.0;
        g_lastDetail = r->message;

        if (r->success) {
          SetStatus(L"OK  " + r->detectedIp + L"  (" + std::to_wstring(r->latencyMs) + L" ms)",
                    COL_SUCCESS);
          AppendResult(L"SUCCESS");
          AppendResult(L"  Exit IP : " + r->detectedIp);
          AppendResult(L"  Latency : " + std::to_wstring(r->latencyMs) + L" ms");
          MaybeAppendDnsLatencyTip();
        } else {
          SetStatus(L"Test failed: " + r->message, COL_ERROR);
          AppendResult(L"FAILED - " + r->message);
        }
        delete r;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_APP_SPEED_DONE: {
      SpeedTestResult* r = reinterpret_cast<SpeedTestResult*>(lParam);
      SetBusy(false);
      if (r) {
        g_hasResult = true;
        g_lastOk = r->success;
        g_lastIp = r->detectedIp;
        g_lastLatency = r->latencyMs;
        g_lastMbps = r->success ? r->downloadMbps : -1.0;
        g_lastDetail = r->message;

        if (r->success) {
          wchar_t speed[64];
          swprintf_s(speed, L"%.2f", r->downloadMbps);
          SetStatus(L"OK  " + std::wstring(speed) + L" Mbps  |  " + r->detectedIp,
                    COL_SUCCESS);
          AppendResult(L"SUCCESS - speed test");
          AppendResult(L"  Exit IP   : " + r->detectedIp);
          AppendResult(L"  Latency   : " + std::to_wstring(r->latencyMs) + L" ms");
          AppendResult(L"  Download  : " + std::wstring(speed) + L" Mbps");
          AppendResult(L"  Bytes     : " + std::to_wstring((long long)r->bytesDownloaded));
          AppendResult(L"  Duration  : " + std::to_wstring(r->downloadMs) + L" ms");
          MaybeAppendDnsLatencyTip();
        } else {
          SetStatus(L"Speed test failed: " + r->message, COL_ERROR);
          AppendResult(L"FAILED - " + r->message);
        }
        delete r;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_APP_LOGIN_DONE: {
      DesktopLoginResult* r = reinterpret_cast<DesktopLoginResult*>(lParam);
      SetBusy(false);
      if (hBtnLogin) EnableWindow(hBtnLogin, TRUE);
      if (r) {
        if (r->ok) {
          // Store BOTH ports from this single login, then fill the form
          SetPortPairAndRefresh(r->socksPort, r->httpPort);

          SetWindowTextW(hHost, r->host.c_str());
          SetWindowTextW(hUser, r->proxyUser.c_str());
          SetWindowTextW(hPass, r->proxyPass.c_str());
          SendMessageW(hComboType, CB_SETCURSEL, 0, 0); // default SOCKS5
          SetPortPairAndRefresh(r->socksPort, r->httpPort); // apply SOCKS port now that type is set
          SendMessageW(hCheckAuth, BM_SETCHECK, BST_CHECKED, 0);
          EnableWindow(hUser, TRUE);
          EnableWindow(hPass, TRUE);

          std::wstring usage;
          if (r->limitMb > 0) {
            usage = L"  " + std::to_wstring((int)r->usedMb) + L"/" +
                    std::to_wstring((int)r->limitMb) + L" MB";
          }
          // Status shows both ports loaded in one go
          SetStatus(L"Loaded " + r->host + L" | SOCKS " + std::to_wstring(g_socksPort) +
                    L" | HTTP " + std::to_wstring(g_httpPort) + usage, COL_SUCCESS);
          AppendResult(L"SUCCESS - account login");
          AppendResult(L"  Email      : " + r->email);
          AppendResult(L"  Proxy host : " + r->host);
          AppendResult(L"  SOCKS port : " + std::to_wstring(g_socksPort));
          AppendResult(L"  HTTP port  : " + std::to_wstring(g_httpPort));
          AppendResult(L"  Username   : " + r->proxyUser);
          AppendResult(L"  Password   : (loaded)");
          if (r->limitMb > 0)
            AppendResult(L"  Plan usage : " + std::to_wstring((int)r->usedMb) + L" / " +
                         std::to_wstring((int)r->limitMb) + L" MB");
          AppendResult(L"Both ports loaded - switch Type (SOCKS5 / HTTP) anytime.");
        } else {
          SetStatus(L"Login failed: " + r->message, COL_ERROR);
          AppendResult(L"FAILED - " + r->message);
        }
        delete r;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_APP_UPDATE_DONE: {
      UpdateCheckResult* r = reinterpret_cast<UpdateCheckResult*>(lParam);
      if (r) {
        if (!r->ok) {
          if (!r->silent) {
            SetStatus(L"Update check failed.", COL_ERROR);
            AppendResult(L"FAILED - " + r->message);
            MessageBoxW(hwnd, r->message.c_str(), L"Check for Updates", MB_OK | MB_ICONWARNING);
          }
        } else if (r->updateAvailable) {
          SetStatus(L"Update available: v" + r->latestVersion, COL_PRIMARY);
          AppendResult(L"Update available: v" + r->latestVersion + L" (current v" + std::wstring(APP_VERSION) + L")");
          if (!r->notes.empty()) AppendResult(L"  Notes: " + r->notes);

          std::wstring msg = L"A new version of ProxyPiTester is available.\r\n\r\n";
          msg += L"Your version:  v" + std::wstring(APP_VERSION) + L"\r\n";
          msg += L"Latest:        v" + r->latestVersion + L"\r\n\r\n";
          if (!r->notes.empty()) {
            msg += r->notes + L"\r\n\r\n";
          }
          msg += L"Open the download page now?";
          if (MessageBoxW(hwnd, msg.c_str(), L"Update Available", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            std::wstring openUrl = r->downloadUrl.empty() ? DEFAULT_DOWNLOAD_URL : r->downloadUrl;
            AppendResult(L"Opening download page...");
            AppendResult(L"  " + openUrl);
            if (!OpenUrlInDefaultBrowser(hwnd, openUrl)) {
              SetStatus(L"Could not open browser - copy the URL from Results.", COL_ERROR);
              AppendResult(L"FAILED - could not open browser. Copy the URL above and paste it into Chrome/Edge.");
              MessageBoxW(hwnd,
                (L"Could not open the browser automatically.\r\n\r\n"
                 L"Copy this URL and open it manually:\r\n\r\n" + openUrl).c_str(),
                L"Check for Updates", MB_OK | MB_ICONWARNING);
            } else {
              SetStatus(L"Opened download page in your browser.", COL_SUCCESS);
            }
          }
        } else {
          if (!r->silent) {
            SetStatus(L"You have the latest version (v" + std::wstring(APP_VERSION) + L").", COL_SUCCESS);
            AppendResult(L"Up to date (v" + std::wstring(APP_VERSION) + L").");
            MessageBoxW(hwnd, r->message.c_str(), L"Check for Updates", MB_OK | MB_ICONINFORMATION);
          }
        }
        delete r;
      }
      return 0;
    }

    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED)
        ApplyLayout(hwnd);
      return 0;

    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      const DWORD style = WS_OVERLAPPEDWINDOW;
      RECT want{ 0, 0, MIN_CLIENT_W, MIN_CLIENT_H };
      AdjustWindowRectEx(&want, style, TRUE, 0);
      mmi->ptMinTrackSize.x = want.right - want.left;
      mmi->ptMinTrackSize.y = want.bottom - want.top;
      // No max size lock — allow maximize and free resize
      return 0;
    }

    case WM_DESTROY: {
      KillTimer(hwnd, TIMER_SESSIONS);
      if (hComboApps)
        g_lastAppSel = (int)SendMessageW(hComboApps, CB_GETCURSEL, 0, 0);
      SaveAppRoutePrefs();
      // Remove SOCKS vmArgs we wrote so next RuneLite start is not stuck on a dead port
      std::wstring clearErr;
      ClearRuneLiteSocksWrap(clearErr);
      ClearRoutedAppSessions();
      StopLocalAuthBridge();
      if (hFontTitle) DeleteObject(hFontTitle);
      if (hFontUi) DeleteObject(hFontUi);
      if (hFontSmall) DeleteObject(hFontSmall);
      if (hFontMono) DeleteObject(hFontMono);
      if (hBrushBg) DeleteObject(hBrushBg);
      if (hBrushCard) DeleteObject(hBrushCard);
      if (hBrushInput) DeleteObject(hBrushInput);
      if (hBrushPrimary) DeleteObject(hBrushPrimary);
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
  INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
  InitCommonControlsEx(&icc);

  HICON hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  HICON hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  if (!hIconBig) hIconBig = LoadIcon(nullptr, IDI_APPLICATION);
  if (!hIconSm) hIconSm = hIconBig;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(COL_BG);
  wc.lpszClassName = L"ProxyPiTesterMain";
  wc.hIcon = hIconBig;
  wc.hIconSm = hIconSm;

  if (!RegisterClassExW(&wc)) return 1;

  // Resizable + maximizable; larger default client area
  const DWORD style = WS_OVERLAPPEDWINDOW;
  RECT wr{ 0, 0, DEFAULT_CLIENT_W, DEFAULT_CLIENT_H };
  AdjustWindowRectEx(&wr, style, TRUE, 0); // TRUE = account for menu bar
  int winW = wr.right - wr.left;
  int winH = wr.bottom - wr.top;

  int sx = GetSystemMetrics(SM_CXSCREEN);
  int sy = GetSystemMetrics(SM_CYSCREEN);
  // Prefer a comfortable size; if screen is smaller, clamp to work area
  if (winW > sx - 40) winW = sx - 40;
  if (winH > sy - 60) winH = sy - 60;
  int x = (sx - winW) / 2;
  int y = (sy - winH) / 2;
  if (y < 20) y = 20;

  HWND hwnd = CreateWindowExW(
    WS_EX_CONTROLPARENT, L"ProxyPiTesterMain",
    L"ProxyPiTester",
    style,
    x, y, winW, winH,
    nullptr, nullptr, hInstance, nullptr);

  if (!hwnd) return 1;

  SetMenu(hwnd, CreateAppMenu());
  DrawMenuBar(hwnd);

  SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    // Enables Tab / Shift+Tab between email, password, and other controls
    if (hMain && IsDialogMessageW(hMain, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return (int)msg.wParam;
}

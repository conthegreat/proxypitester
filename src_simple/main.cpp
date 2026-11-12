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

#include <uxtheme.h>
#include <dwmapi.h>
#include <dbghelp.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- App version & update feed (public GitHub — no credentials required) ----
// Bump APP_VERSION when you ship a new build. Host update.json on a public repo/raw URL.
static const wchar_t* APP_VERSION = L"1.3.5";
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

// ---- ProxyPi brand palette (local visual pass; same product, richer contrast) ----
static const COLORREF COL_BG         = RGB(8, 12, 20);
static const COLORREF COL_BG_DEEP    = RGB(14, 20, 34);     // input wells
static const COLORREF COL_CARD       = RGB(17, 25, 41);
static const COLORREF COL_PRIMARY    = RGB(34, 211, 238);   // bright cyan
static const COLORREF COL_PRIMARY_D  = RGB(56, 224, 248);   // hover
static const COLORREF COL_PRIMARY_DD = RGB(8, 168, 196);    // pressed
static const COLORREF COL_ON_PRIMARY = RGB(7, 18, 28);      // text on cyan
static const COLORREF COL_TEXT       = RGB(241, 245, 249);
static const COLORREF COL_TEXT_BODY  = RGB(203, 213, 225);
static const COLORREF COL_TEXT_DIM   = RGB(148, 163, 184);
static const COLORREF COL_BORDER     = RGB(51, 68, 92);
static const COLORREF COL_CARD_LINE  = RGB(96, 122, 158);   // panel outline, lighter than the canvas
static const COLORREF COL_FIELD_LINE = RGB(32, 46, 68);     // recessed control outline, darker than the panel
static const COLORREF COL_SUCCESS    = RGB(52, 211, 153);
static const COLORREF COL_ERROR      = RGB(248, 113, 113);
static const COLORREF COL_INDIGO     = RGB(129, 140, 248);

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
#define ID_CHECK_REMEMBER  2040
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
#define ID_BTN_QUIT_APP      2032
#define ID_BTN_VERIFY_IP     2033
#define ID_BTN_COPY_REPORT   2034
#define ID_COMBO_PROFILE     2035
#define ID_BTN_SAVE_PROFILE  2036
#define ID_BTN_RUN_PROFILE   2037
#define ID_BTN_DEL_PROFILE   2038
#define ID_CHECK_TRAY        2039

// Menu IDs
#define IDM_FILE_EXIT       3001
#define IDM_FILE_TRAY       3002
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
#define IDM_TRAY_SHOW        3101
#define IDM_TRAY_EXIT        3102

// Custom messages from worker threads
#define WM_APP_TEST_DONE    (WM_APP + 10)
#define WM_APP_SPEED_DONE   (WM_APP + 11)
#define WM_APP_UPDATE_DONE  (WM_APP + 12)
#define WM_APP_LOGIN_DONE   (WM_APP + 13)
#define WM_APP_ROUTE_DONE   (WM_APP + 20)
#define WM_APP_VERIFY_DONE  (WM_APP + 21)
#define WM_APP_DL_UPDATE    (WM_APP + 22)
#define WM_TRAYICON         (WM_APP + 60)

#define TIMER_SESSIONS  77
#define TIMER_HEALTH    78
#define TRAY_ICON_ID    1

static HWND hMain = nullptr;
static HMENU g_menuBar = nullptr;
static int g_menuHot = 0; // 1 = File, 2 = Help
static HWND hComboType = nullptr;
static HWND hHost = nullptr, hPort = nullptr;
static HWND hCheckAuth = nullptr;
static HWND hUser = nullptr, hPass = nullptr;
static HWND hBtnTest = nullptr, hBtnSpeed = nullptr;
static HWND hBtnSave = nullptr, hBtnLoad = nullptr, hBtnSite = nullptr;
static HWND hResult = nullptr;
static HWND hStatus = nullptr;
static HWND hAcctEmail = nullptr, hAcctPass = nullptr, hBtnLogin = nullptr;
static HWND hCheckRemember = nullptr;
static HWND hLblRemember = nullptr; // separate label so text matches Email/Password colour
static bool g_rememberAccount = false;
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
static HWND hBtnQuitApp = nullptr;
static HWND hBtnVerifyIp = nullptr;
static HWND hBtnCopyReport = nullptr;
static HWND hComboProfile = nullptr;
static HWND hBtnSaveProfile = nullptr;
static HWND hBtnRunProfile = nullptr;
static HWND hBtnDelProfile = nullptr;
static HWND hCheckTray = nullptr;
static std::wstring g_chromePath; // empty = auto-detect
static std::wstring g_customAppPath; // last browsed Chromium-like exe
static int g_lastAppSel = 0;         // combo index remembered in ini
// Default OFF — minimize-to-tray made the app look "crashed" for many users
static bool g_minimizeToTray = false;
static bool g_trayAdded = false;
static NOTIFYICONDATAW g_nid{};
static UINT g_dpi = 96;
static bool g_autoTestAfterLogin = true; // product UI #5

static HFONT hFontUi = nullptr;
static HFONT hFontTitle = nullptr;
static HFONT hFontSmall = nullptr;
static HFONT hFontMono = nullptr;
static HFONT hFontBtn = nullptr;
static HFONT hFontMetric = nullptr;
static HFONT hFontSection = nullptr;
static HICON g_headerIcon = nullptr;
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
static const int DEFAULT_CLIENT_W = 960;
static const int DEFAULT_CLIENT_H = 920;
static const int MIN_CLIENT_W = 700;
// Short windows scroll instead of squashing — keep a usable min track height
static const int MIN_CLIENT_H = 520;
static const int PAD = 24;
// Largest image in ProxyPiTester.ico. Drawn 1:1 so the header mark stays sharp.
static const int HEADER_ICON_PX = 48;
// Painted command strip. Replaces the native menu bar, which stays white on Windows 11.
static const int MENU_STRIP_H = 34;
// Side-by-side account | config when client is at least this wide
static const int SIDE_BY_SIDE_MIN_W = 900;
// Comfortable preferred heights for list/results when space is tight (scroll covers rest)
static const int PREF_LIST_H = 140;
static const int PREF_RES_H = 120;

// Fixed control widths (do not stretch with window)
static const int BTN_PRIMARY_W = 140;
static const int BTN_SECONDARY_W = 96;
static const int BTN_LOGIN_W = 260;
static const int BTN_LAUNCH_W = 110;
static const int BTN_REFRESH_W = 96;
static const int COMBO_APPS_W = 230;
static const int BTN_COPY_W = 100;
static const int BTN_URL_W = 100;

// Runtime layout — only ApplyLayout writes these; WM_PAINT must only read them.
// All Y coordinates are in *document* space (content coords). Subtract scrollY for screen.
struct UiLayout {
  int clientW = DEFAULT_CLIENT_W;
  int clientH = DEFAULT_CLIENT_H;
  int contentH = DEFAULT_CLIENT_H; // full document height (may exceed clientH)
  int scrollY = 0;                 // vertical scroll offset (document -> screen)
  int cardX = 20;
  int cardW = DEFAULT_CLIENT_W - 40;
  bool sideBySide = false;
  int acctLeft = 20, acctTop = 112, acctW = 0, acctBot = 250;
  int cfgLeft = 20, cfgTop = 262, cfgW = 0, cfgBot = 470;
  int metricsY = 482;
  int metricsH = 72;
  int testBtnY = 572, secBtnY = 624;
  int routeTop = 670, routeBot = 900;
  int resTop = 912, resBot = 1000;
  int statusY = 1010, footerY = 1055;
  int routeToolbarH = 154; // chrome above session list inside ACTIVE APPS card
  bool narrow = false;
  bool needScroll = false;
};
static UiLayout g_lay;
static bool g_inLayout = false;

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

// Defined near wWinMain; declared early for WM_CREATE logging
static void StartupLog(const wchar_t* line);

static std::wstring GetIniPath() {
  return GetExeDir() + L"\\ProxyPiTester.ini";
}

static void SaveAccountCredentials() {
  std::wstring path = GetIniPath();
  WritePrivateProfileStringW(L"Account", L"Remember", g_rememberAccount ? L"1" : L"0", path.c_str());
  if (!g_rememberAccount) {
    // Clear stored secrets when user opts out
    WritePrivateProfileStringW(L"Account", L"Email", L"", path.c_str());
    WritePrivateProfileStringW(L"Account", L"Password", L"", path.c_str());
    return;
  }
  wchar_t email[512] = {}, pass[512] = {};
  if (hAcctEmail) GetWindowTextW(hAcctEmail, email, 512);
  if (hAcctPass) GetWindowTextW(hAcctPass, pass, 512);
  WritePrivateProfileStringW(L"Account", L"Email", email, path.c_str());
  WritePrivateProfileStringW(L"Account", L"Password", pass, path.c_str());
}

static void LoadAccountCredentials() {
  std::wstring path = GetIniPath();
  g_rememberAccount = GetPrivateProfileIntW(L"Account", L"Remember", 0, path.c_str()) != 0;
  if (hCheckRemember)
    SendMessageW(hCheckRemember, BM_SETCHECK, g_rememberAccount ? BST_CHECKED : BST_UNCHECKED, 0);
  if (!g_rememberAccount) return;
  wchar_t email[512] = {}, pass[512] = {};
  GetPrivateProfileStringW(L"Account", L"Email", L"", email, 512, path.c_str());
  GetPrivateProfileStringW(L"Account", L"Password", L"", pass, 512, path.c_str());
  if (hAcctEmail && email[0]) SetWindowTextW(hAcctEmail, email);
  if (hAcctPass && pass[0]) SetWindowTextW(hAcctPass, pass);
}

static bool SaveIni(const ProxyConfig& cfg) {
  std::wstring path = GetIniPath();
  WritePrivateProfileStringW(L"Proxy", L"Type", cfg.type == ProxyType::SOCKS5 ? L"SOCKS5" : L"HTTP", path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Host", cfg.host.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Port", std::to_wstring(cfg.port).c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"SocksPort", std::to_wstring(g_socksPort).c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"HttpPort", std::to_wstring(g_httpPort).c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"UseAuth", cfg.useAuth ? L"1" : L"0", path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Username", cfg.username.c_str(), path.c_str());
  WritePrivateProfileStringW(L"Proxy", L"Password", cfg.password.c_str(), path.c_str());
  WritePrivateProfileStringW(L"AppRoute", L"ChromePath", g_chromePath.c_str(), path.c_str());
  // Also persist account login if "Remember me" is checked
  if (hCheckRemember)
    g_rememberAccount = (SendMessageW(hCheckRemember, BM_GETCHECK, 0, 0) == BST_CHECKED);
  SaveAccountCredentials();
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
  g_socksPort = GetPrivateProfileIntW(L"Proxy", L"SocksPort", cfg.port > 0 ? cfg.port : 18721, path.c_str());
  g_httpPort = GetPrivateProfileIntW(L"Proxy", L"HttpPort", 58920, path.c_str());
  cfg.useAuth = GetPrivateProfileIntW(L"Proxy", L"UseAuth", 1, path.c_str()) != 0;
  GetPrivateProfileStringW(L"Proxy", L"Username", L"", buf, 512, path.c_str());
  cfg.username = buf;
  GetPrivateProfileStringW(L"Proxy", L"Password", L"", buf, 512, path.c_str());
  cfg.password = buf;
  wchar_t chromeBuf[MAX_PATH] = {};
  GetPrivateProfileStringW(L"AppRoute", L"ChromePath", L"", chromeBuf, MAX_PATH, path.c_str());
  g_chromePath = chromeBuf;
  LoadAccountCredentials();
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
// Stronger kill+relaunch for Electron apps (#19)
static bool ConfirmRelaunchIfRunning(const wchar_t* appName, const wchar_t* imageExe) {
  if (!imageExe || !imageExe[0] || !IsImageRunning(imageExe)) return true;
  std::wstring msg = appName;
  msg += L" is already running.\r\n\r\n";
  msg += L"Proxy flags only apply on a fresh start.\r\n\r\n";
  msg += L"Yes = close all ";
  msg += appName;
  msg += L" processes, then launch proxied\r\n";
  msg += L"No  = cancel\r\n";
  msg += L"Cancel = try launch without closing (often still on home IP)";
  int r = MessageBoxW(hMain, msg.c_str(), L"App already running",
                      MB_YESNOCANCEL | MB_ICONWARNING);
  if (r == IDCANCEL) return true; // launch anyway (old behavior)
  if (r == IDNO) return false;
  int n = TerminateProcessesByImage(imageExe, 5000);
  // Common second images
  if (_wcsicmp(imageExe, L"Discord.exe") == 0)
    n += TerminateProcessesByImage(L"Update.exe", 1000);
  Sleep(500);
  AppendResult(L"Closed " + std::to_wstring(n) + L" process(es) of " + appName);
  return true;
}

static void LoadAppRoutePrefs() {
  std::wstring path = GetIniPath();
  g_lastAppSel = GetPrivateProfileIntW(L"AppRoute", L"LastAppSel", 0, path.c_str());
  // TrayPrefV < 2: force tray OFF once (old default hid the window and looked like a crash)
  int trayPrefV = GetPrivateProfileIntW(L"AppRoute", L"TrayPrefV", 0, path.c_str());
  if (trayPrefV < 2) {
    g_minimizeToTray = false;
    WritePrivateProfileStringW(L"AppRoute", L"MinimizeToTray", L"0", path.c_str());
    WritePrivateProfileStringW(L"AppRoute", L"TrayPrefV", L"2", path.c_str());
  } else {
    g_minimizeToTray = GetPrivateProfileIntW(L"AppRoute", L"MinimizeToTray", 0, path.c_str()) != 0;
  }
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
  if (hCheckTray) SendMessageW(hCheckTray, BM_SETCHECK, g_minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void SaveAppRoutePrefs() {
  std::wstring path = GetIniPath();
  WritePrivateProfileStringW(L"AppRoute", L"LastAppSel",
                             std::to_wstring(g_lastAppSel).c_str(), path.c_str());
  WritePrivateProfileStringW(L"AppRoute", L"MinimizeToTray",
                             g_minimizeToTray ? L"1" : L"0", path.c_str());
  wchar_t url[1024] = {};
  if (hEditOpenUrl) GetWindowTextW(hEditOpenUrl, url, 1024);
  WritePrivateProfileStringW(L"AppRoute", L"LastUrl", url, path.c_str());
  if (!g_customAppPath.empty())
    WritePrivateProfileStringW(L"AppRoute", L"CustomApp", g_customAppPath.c_str(), path.c_str());
}

static void SetStatus(const std::wstring& text, COLORREF color = COL_TEXT_DIM);
static void AppendResult(const std::wstring& text);

// ---- Profiles (product #1) ----
// Stored as [Profiles] List=Name1|Name2  and [Profile_Name1] Apps=Chrome,Postman
static std::vector<std::wstring> SplitPipe(const std::wstring& s) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(L'|', i);
    if (j == std::wstring::npos) j = s.size();
    std::wstring part = s.substr(i, j - i);
    while (!part.empty() && part[0] == L' ') part.erase(part.begin());
    while (!part.empty() && part.back() == L' ') part.pop_back();
    if (!part.empty()) out.push_back(part);
    i = j + 1;
  }
  return out;
}

static std::wstring JoinPipe(const std::vector<std::wstring>& v) {
  std::wstring s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += L'|';
    s += v[i];
  }
  return s;
}

static std::vector<std::wstring> LoadProfileNames() {
  wchar_t buf[2048] = {};
  GetPrivateProfileStringW(L"Profiles", L"List", L"", buf, 2048, GetIniPath().c_str());
  return SplitPipe(buf);
}

static void SaveProfileNames(const std::vector<std::wstring>& names) {
  WritePrivateProfileStringW(L"Profiles", L"List", JoinPipe(names).c_str(), GetIniPath().c_str());
}

static std::wstring ProfileSection(const std::wstring& name) {
  return L"Profile_" + name;
}

static void RefreshProfileCombo() {
  if (!hComboProfile) return;
  SendMessageW(hComboProfile, CB_RESETCONTENT, 0, 0);
  auto names = LoadProfileNames();
  for (const auto& n : names)
    SendMessageW(hComboProfile, CB_ADDSTRING, 0, (LPARAM)n.c_str());
  if (!names.empty()) SendMessageW(hComboProfile, CB_SETCURSEL, 0, 0);
}

static std::wstring AppNameFromComboIndex(int idx) {
  // Must match CB_ADDSTRING order in WM_CREATE
  static const wchar_t* kNames[] = {
    L"Chrome", L"Edge", L"Brave", L"Firefox", L"Discord",
    L"Opera", L"Vivaldi", L"Slack", L"Teams", L"VS Code", L"Cursor",
    L"Postman", L"Thunderbird", L"Spotify", L"RuneLite", L"Jagex", L"Browse"
  };
  if (idx < 0 || idx >= (int)(sizeof(kNames) / sizeof(kNames[0]))) return {};
  return kNames[idx];
}

static void DoSaveProfile() {
  auto names = LoadProfileNames();
  std::wstring defaultName = names.empty() ? L"Default" : names[0];
  // Simple input via fixed dialog-less approach: use current combo edit or prompt
  wchar_t nameBuf[128] = {};
  if (hComboProfile) {
    int sel = (int)SendMessageW(hComboProfile, CB_GETCURSEL, 0, 0);
    if (sel >= 0) SendMessageW(hComboProfile, CB_GETLBTEXT, sel, (LPARAM)nameBuf);
  }
  // Ask for name
  std::wstring prompt = L"Save profile as (name only, no spaces preferred):\r\n\r\n";
  prompt += L"Current selection will save apps from Active Apps if any,\r\n";
  prompt += L"otherwise the currently selected dropdown app.";
  // Use a simple InputBox substitute: MessageBox can't input. Use GetWindowText from combo if CBS_DROPDOWN.
  // Our combo is DROPDOWNLIST — so use last known or "Profile1"
  std::wstring name = nameBuf;
  if (name.empty()) {
    name = L"Profile" + std::to_wstring((int)names.size() + 1);
  }
  // Collect apps: prefer live sessions, else current combo app
  std::vector<std::wstring> apps;
  auto sessions = GetRoutedAppSessions();
  for (const auto& s : sessions) {
    if (s.name != L"Browse") apps.push_back(s.name);
  }
  if (apps.empty() && hComboApps) {
    int sel = (int)SendMessageW(hComboApps, CB_GETCURSEL, 0, 0);
    std::wstring a = AppNameFromComboIndex(sel);
    if (!a.empty() && a != L"Browse") apps.push_back(a);
  }
  if (apps.empty()) {
    MessageBoxW(hMain, L"Open at least one app (or select one in the dropdown) before saving a profile.",
                L"Profiles", MB_OK | MB_ICONINFORMATION);
    return;
  }
  // Confirm name
  std::wstring conf = L"Save profile \"" + name + L"\" with:\r\n\r\n";
  for (const auto& a : apps) conf += L"  - " + a + L"\r\n";
  conf += L"\r\nOK to save?";
  if (MessageBoxW(hMain, conf.c_str(), L"Save profile", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
    return;

  bool found = false;
  for (const auto& n : names) if (n == name) found = true;
  if (!found) names.push_back(name);
  SaveProfileNames(names);

  std::wstring appsJoined;
  for (size_t i = 0; i < apps.size(); ++i) {
    if (i) appsJoined += L",";
    appsJoined += apps[i];
  }
  WritePrivateProfileStringW(ProfileSection(name).c_str(), L"Apps", appsJoined.c_str(), GetIniPath().c_str());
  RefreshProfileCombo();
  // select saved
  int idx = (int)SendMessageW(hComboProfile, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name.c_str());
  if (idx >= 0) SendMessageW(hComboProfile, CB_SETCURSEL, idx, 0);
  AppendResult(L"Profile saved: " + name + L" (" + appsJoined + L")");
  SetStatus(L"Profile saved: " + name, COL_SUCCESS);
}

static void DoDeleteProfile() {
  if (!hComboProfile) return;
  int sel = (int)SendMessageW(hComboProfile, CB_GETCURSEL, 0, 0);
  if (sel < 0) return;
  wchar_t nameBuf[128] = {};
  SendMessageW(hComboProfile, CB_GETLBTEXT, sel, (LPARAM)nameBuf);
  std::wstring name = nameBuf;
  if (name.empty()) return;
  if (MessageBoxW(hMain, (L"Delete profile \"" + name + L"\"?").c_str(),
                  L"Delete profile", MB_YESNO | MB_ICONWARNING) != IDYES)
    return;
  auto names = LoadProfileNames();
  std::vector<std::wstring> keep;
  for (const auto& n : names) if (n != name) keep.push_back(n);
  SaveProfileNames(keep);
  WritePrivateProfileStringW(ProfileSection(name).c_str(), nullptr, nullptr, GetIniPath().c_str());
  RefreshProfileCombo();
  AppendResult(L"Profile deleted: " + name);
}

static void DoRunProfileByName(const std::wstring& name); // fwd

static void DoRunSelectedProfile() {
  if (!hComboProfile) return;
  int sel = (int)SendMessageW(hComboProfile, CB_GETCURSEL, 0, 0);
  if (sel < 0) {
    MessageBoxW(hMain, L"Select a profile first (or Save one from Active Apps).", L"Profiles", MB_OK);
    return;
  }
  wchar_t nameBuf[128] = {};
  SendMessageW(hComboProfile, CB_GETLBTEXT, sel, (LPARAM)nameBuf);
  DoRunProfileByName(nameBuf);
}

// ---- Tray (#2) ----
static void TrayRemove() {
  if (!g_trayAdded) return;
  Shell_NotifyIconW(NIM_DELETE, &g_nid);
  g_trayAdded = false;
}

static void TrayAdd(HWND hwnd, bool balloon = true) {
  if (g_trayAdded) {
    if (balloon) {
      g_nid.uFlags = NIF_INFO | NIF_TIP | NIF_ICON | NIF_MESSAGE;
      g_nid.dwInfoFlags = NIIF_INFO;
      wcsncpy_s(g_nid.szInfoTitle, L"ProxyPiTester is still running", _TRUNCATE);
      wcsncpy_s(g_nid.szInfo,
                L"Minimized to the system tray (near the clock). Click the icon to restore.",
                _TRUNCATE);
      Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    return;
  }
  ZeroMemory(&g_nid, sizeof(g_nid));
  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = hwnd;
  g_nid.uID = TRAY_ICON_ID;
  g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | (balloon ? NIF_INFO : 0);
  g_nid.uCallbackMessage = WM_TRAYICON;
  g_nid.hIcon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0);
  if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcsncpy_s(g_nid.szTip, L"ProxyPiTester - click to restore", _TRUNCATE);
  if (balloon) {
    g_nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, L"ProxyPiTester is still running", _TRUNCATE);
    wcsncpy_s(g_nid.szInfo,
              L"Minimized to the system tray (near the clock). Click the icon to restore.",
              _TRUNCATE);
  }
  if (Shell_NotifyIconW(NIM_ADD, &g_nid)) g_trayAdded = true;
}

static void TrayShowWindow(HWND hwnd) {
  ShowWindow(hwnd, SW_SHOW);
  ShowWindow(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
  StartupLog(L"restored from tray");
}

// ---- Session status string (#4) ----
static std::wstring SessionStatusLabel(const RoutedAppSession& s) {
  DWORD ageMs = GetTickCount() - s.startTick;
  if (s.connCount > 0) return L"OK";
  if (ageMs < 15000) return L"STARTING";
  if (s.alive && s.processCount > 0) return L"NO CONNS";
  return L"STALE";
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

  SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)L"");
  {
    std::wstring hdr =
      PadCell(L"APP", 12) + L" " +
      PadCell(L"STATUS", 9) + L" " +
      PadCell(L"PID", 8, true) + L" " +
      PadCell(L"CONNS", 5, true) + L" " +
      PadCell(L"BRIDGE", 6, true) + L"  " +
      L"METHOD";
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)hdr.c_str());
    SendMessageW(hSessionList, LB_ADDSTRING, 0,
      (LPARAM)L"------------ --------- -------- ----- ------  ----------------");
  }

  for (const auto& s : sessions) {
    std::wstring st = SessionStatusLabel(s);
    std::wstring line =
      PadCell(s.name, 12) + L" " +
      PadCell(st, 9) + L" " +
      PadCell(std::to_wstring(s.pid), 8, true) + L" " +
      PadCell(s.connCount > 0 ? std::to_wstring(s.connCount) : L"-", 5, true) + L" " +
      PadCell(s.bridgePort > 0 ? std::to_wstring(s.bridgePort) : L"-", 6, true) + L"  " +
      (s.method.empty() ? L"-" : s.method);
    SendMessageW(hSessionList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
  }
}

static ProxyConfig ReadConfigFromUI(); // fwd
static void DoTest(); // fwd
static void DoLaunchSelectedApp(); // fwd

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
  msg += L"Support: support@proxypi.co.uk\r\n\r\n";
  msg += L"Dev build ";
  msg += APP_VERSION;
  msg += L" (unsigned - SmartScreen may warn until production code-signing).\r\n";
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

static void AddMenuItem(HMENU menu, UINT id, const wchar_t* text) {
  MENUITEMINFOW mi{};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_DATA | MIIM_STRING;
  mi.fType = MFT_OWNERDRAW;
  mi.wID = id;
  mi.dwTypeData = const_cast<LPWSTR>(text);
  mi.cch = (UINT)wcslen(text);
  mi.dwItemData = (ULONG_PTR)text;
  InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mi);
}

static void AddMenuSep(HMENU menu) {
  MENUITEMINFOW mi{};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA;
  mi.fType = MFT_OWNERDRAW;
  mi.wID = 0;
  mi.dwItemData = 0;
  InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mi);
}

static void AddMenuPopup(HMENU menu, HMENU sub, const wchar_t* text) {
  MENUITEMINFOW mi{};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_SUBMENU | MIIM_FTYPE | MIIM_DATA | MIIM_STRING;
  mi.fType = MFT_OWNERDRAW;
  mi.hSubMenu = sub;
  mi.dwTypeData = const_cast<LPWSTR>(text);
  mi.cch = (UINT)wcslen(text);
  mi.dwItemData = (ULONG_PTR)text;
  InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mi);
}

static HMENU CreateAppMenu() {
  HMENU hMenubar = CreateMenu();
  HMENU hFile = CreatePopupMenu();
  HMENU hHelp = CreatePopupMenu();
  HMENU hAppRoute = CreatePopupMenu();

  AddMenuItem(hFile, IDM_FILE_EXIT, L"Exit");

  AddMenuItem(hAppRoute, IDM_HELP_OPEN_CHROME, L"Open Chrome (proxied)");
  AddMenuItem(hAppRoute, IDM_HELP_OPEN_EDGE, L"Open Edge (proxied)");
  AddMenuItem(hAppRoute, IDM_HELP_OPEN_FIREFOX, L"Open Firefox (proxied)");
  AddMenuItem(hAppRoute, IDM_HELP_OPEN_RUNELITE, L"Open RuneLite (proxied)");
  AddMenuItem(hAppRoute, IDM_HELP_OPEN_JAGEX, L"Open Jagex + RuneLite wrap");
  AddMenuSep(hAppRoute);
  AddMenuItem(hAppRoute, IDM_HELP_SET_CHROME, L"Set Chrome path...");
  AddMenuItem(hAppRoute, IDM_HELP_CLEAR_CHROME, L"Reset Chrome path (auto-detect)");
  AddMenuSep(hAppRoute);
  AddMenuItem(hAppRoute, IDM_HELP_APPROUTE, L"About app routing");

  AddMenuItem(hHelp, IDM_HELP_UPDATE, L"Check for Updates...");
  AddMenuItem(hHelp, IDM_HELP_WEBSITE, L"Visit Website");
  AddMenuPopup(hHelp, hAppRoute, L"App routing");
  AddMenuSep(hHelp);
  AddMenuItem(hHelp, IDM_HELP_ABOUT, L"About ProxyPiTester");

  AddMenuPopup(hMenubar, hFile, L"File");
  AddMenuPopup(hMenubar, hHelp, L"Help");
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
        msg += L" Tip: GET https://api.ipify.org should match Test Proxy exit IP.";
      } else if (name == L"VS Code" || name == L"Cursor") {
        msg += L" Tip: ProxyPi profile forces SOCKS (extensions from default folder).";
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
  ofn.lpstrTitle = L"Select Chromium or Electron app (Chrome, Brave, Discord, Opera...)";
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
      L"This is not a Windows right-click on links yet - it is in-app:\r\n"
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

static std::wstring BuildFullSessionReport() {
  std::wstring report;
  report += L"ProxyPiTester " + std::wstring(APP_VERSION) + L" session report\r\n";
  report += L"================================\r\n";
  report += L"Exit IP: " + (g_lastIp.empty() ? L"(unknown - run Test/Verify)" : g_lastIp) + L"\r\n";
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
  report += L"Bridge: ";
  report += (bp > 0) ? (L"UP 127.0.0.1:" + std::to_wstring(bp)) : L"OFF";
  report += L"\r\n";
  if (IsRuneLiteSocksWrapActive())
    report += L"RuneLite wrap: armed port " + std::to_wstring(GetRuneLiteSocksWrapPort()) + L"\r\n";
  ProxyConfig cfg = ReadConfigFromUI();
  if (!cfg.host.empty()) {
    report += L"Upstream: " + cfg.host + L":" + std::to_wstring(cfg.port) + L"\r\n";
  }
  auto sessions = GetRoutedAppSessions();
  report += L"Active apps (" + std::to_wstring(sessions.size()) + L"):\r\n";
  if (sessions.empty()) report += L"  (none)\r\n";
  for (const auto& s : sessions) {
    report += L"  - " + s.name + L" [" + SessionStatusLabel(s) + L"] PID=" +
              std::to_wstring(s.pid) + L" conns=" + std::to_wstring(s.connCount) +
              L" method=" + s.method + L"\r\n";
  }
  report += L"================================\r\n";
  return report;
}

static bool ClipboardSetText(const std::wstring& text) {
  if (!OpenClipboard(hMain)) return false;
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!hMem) { CloseClipboard(); return false; }
  void* p = GlobalLock(hMem);
  if (!p) { GlobalFree(hMem); CloseClipboard(); return false; }
  memcpy(p, text.c_str(), bytes);
  GlobalUnlock(hMem);
  SetClipboardData(CF_UNICODETEXT, hMem);
  CloseClipboard();
  return true;
}

static void DoCopyExitIp() {
  std::wstring report = BuildFullSessionReport();
  if (!ClipboardSetText(report)) {
    SetStatus(L"Could not open clipboard.", COL_ERROR);
    return;
  }
  if (g_hasResult && !g_lastIp.empty()) {
    SetStatus(L"Copied exit IP " + g_lastIp + L" + full session report.", COL_SUCCESS);
    AppendResult(L"Copied report (exit IP " + g_lastIp + L").");
  } else {
    SetStatus(L"Copied session report (run Test/Verify for exit IP).", COL_TEXT_DIM);
    AppendResult(L"Copied full session report.");
  }
}

static void DoCopyFullReport() {
  DoCopyExitIp();
}

static void DoQuitSelectedApp() {
  if (!hSessionList) return;
  int sel = (int)SendMessageW(hSessionList, LB_GETCURSEL, 0, 0);
  if (sel < 0) {
    MessageBoxW(hMain, L"Select an app row in Active Apps first.", L"Quit app", MB_OK | MB_ICONINFORMATION);
    return;
  }
  wchar_t line[512] = {};
  SendMessageW(hSessionList, LB_GETTEXT, sel, (LPARAM)line);
  std::wstring s = line;
  // Skip header rows
  if (s.find(L"Bridge") == 0 || s.find(L"Wrap") == 0 || s.find(L"APP") == 0 ||
      s.find(L"---") == 0 || s.empty() || s.find(L"(no routed") != std::wstring::npos) {
    MessageBoxW(hMain, L"Select a data row (an app name), not a header.", L"Quit app", MB_OK);
    return;
  }
  // First token is app name (padded)
  std::wstring app = s.substr(0, 12);
  while (!app.empty() && app.back() == L' ') app.pop_back();
  if (app.empty()) return;
  if (MessageBoxW(hMain, (L"Quit all processes for \"" + app + L"\"?").c_str(),
                  L"Quit app", MB_YESNO | MB_ICONWARNING) != IDYES)
    return;
  int n = TerminateRoutedApp(app);
  AppendResult(L"Quit " + app + L": terminated " + std::to_wstring(n) + L" process(es).");
  SetStatus(L"Quit " + app, COL_SUCCESS);
  RefreshSessionList();
}

static void DoVerifyExitIp() {
  ProxyConfig cfg;
  if (!RequireProxyForRoute(cfg)) return;
  SetStatus(L"Verifying exit IP through proxy...", COL_PRIMARY);
  AppendResult(L"--- Verify exit IP ---");
  std::thread([cfg]() {
    // Direct TestProxy through configured upstream (same as Test Proxy)
    ProxyTestResult r = TestProxy(cfg, "api.ipify.org", 80);
    std::wstring msg;
    if (r.success) {
      msg = L"VERIFY OK  Exit IP " + r.detectedIp + L"  (" + std::to_wstring(r.latencyMs) + L" ms)";
      if (!g_lastIp.empty() && g_lastIp != r.detectedIp)
        msg += L"  [differs from last test " + g_lastIp + L"]";
    } else {
      msg = L"VERIFY FAILED - " + r.message;
    }
    auto* heap = new ProxyTestResult(r);
    // Stash message in result.message for UI
    heap->message = msg;
    PostMessageW(hMain, WM_APP_VERIFY_DONE, r.success ? 1 : 0, (LPARAM)heap);
  }).detach();
}

static void LaunchAppByFriendlyName(const std::wstring& name) {
  // Map to combo index and reuse DoLaunchSelectedApp path
  static const wchar_t* kNames[] = {
    L"Chrome", L"Edge", L"Brave", L"Firefox", L"Discord",
    L"Opera", L"Vivaldi", L"Slack", L"Teams", L"VS Code", L"Cursor",
    L"Postman", L"Thunderbird", L"Spotify", L"RuneLite", L"Jagex"
  };
  int idx = -1;
  for (int i = 0; i < (int)(sizeof(kNames) / sizeof(kNames[0])); ++i) {
    if (_wcsicmp(name.c_str(), kNames[i]) == 0) { idx = i; break; }
  }
  if (idx < 0) {
    AppendResult(L"Profile app not recognized: " + name);
    return;
  }
  if (hComboApps) SendMessageW(hComboApps, CB_SETCURSEL, idx, 0);
  g_lastAppSel = idx;
  DoLaunchSelectedApp();
}

static void DoRunProfileByName(const std::wstring& name) {
  if (name.empty()) return;
  wchar_t appsBuf[1024] = {};
  GetPrivateProfileStringW(ProfileSection(name).c_str(), L"Apps", L"", appsBuf, 1024, GetIniPath().c_str());
  if (!appsBuf[0]) {
    MessageBoxW(hMain, L"Profile has no apps saved.", L"Profiles", MB_OK | MB_ICONWARNING);
    return;
  }
  AppendResult(L"--- Run profile: " + name + L" ---");
  AppendResult(L"  Apps: " + std::wstring(appsBuf));
  // Split commas
  std::wstring list = appsBuf;
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find(L',', i);
    if (j == std::wstring::npos) j = list.size();
    std::wstring app = list.substr(i, j - i);
    while (!app.empty() && app[0] == L' ') app.erase(app.begin());
    while (!app.empty() && app.back() == L' ') app.pop_back();
    if (!app.empty()) {
      AppendResult(L"  Launching " + app + L"...");
      LaunchAppByFriendlyName(app);
      Sleep(800); // brief stagger so bridge is shared cleanly
    }
    i = j + 1;
  }
  SetStatus(L"Profile launched: " + name, COL_SUCCESS);
}

struct UpdateDownloadResult {
  bool ok = false;
  std::wstring message;
  std::wstring folder;
};

static void DoDownloadUpdateZip(const std::wstring& url) {
  AppendResult(L"Downloading update package...");
  std::thread([url]() {
    UpdateDownloadResult r;
    std::wstring useUrl = url;
    // If URL is releases/latest page, use known asset pattern from latest version is hard —
    // prefer direct zip if present; else open page only.
    if (useUrl.find(L".zip") == std::wstring::npos) {
      useUrl = L"https://github.com/conthegreat/proxypitester/releases/latest/download/ProxyPiTester-latest-win64.zip";
      // Fallback: user-facing latest release page open if download fails
    }

    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring dir = std::wstring(local) + L"\\ProxyPiTester\\Updates";
    // create dirs
    CreateDirectoryW((std::wstring(local) + L"\\ProxyPiTester").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    // Parse host/path from https URL for WinHTTP
    std::wstring host, path;
    if (useUrl.rfind(L"https://", 0) == 0) {
      size_t slash = useUrl.find(L'/', 8);
      if (slash != std::wstring::npos) {
        host = useUrl.substr(8, slash - 8);
        path = useUrl.substr(slash);
      }
    }
    std::string body;
    std::wstring err;
    // HttpGetHttps only returns string body - for zip binary we need raw download
    // Use WinHTTP here for binary
    bool okDl = false;
    std::wstring outFile = dir + L"\\ProxyPiTester-update.zip";
    HINTERNET hS = WinHttpOpen(L"ProxyPiTester/1.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hS && !host.empty()) {
      HINTERNET hC = WinHttpConnect(hS, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
      if (hC) {
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hR) {
          // Follow redirects for latest/download
          DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
          WinHttpSetOption(hR, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
          if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hR, nullptr)) {
            HANDLE hF = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hF != INVALID_HANDLE_VALUE) {
              okDl = true;
              for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(hR, buf.data(), avail, &read) || read == 0) break;
                DWORD wr = 0;
                WriteFile(hF, buf.data(), read, &wr, nullptr);
              }
              CloseHandle(hF);
              // Sanity: zip should be non-tiny
              WIN32_FILE_ATTRIBUTE_DATA fad{};
              if (GetFileAttributesExW(outFile.c_str(), GetFileExInfoStandard, &fad)) {
                if (fad.nFileSizeLow < 10000 && fad.nFileSizeHigh == 0) okDl = false;
              }
            }
          }
          WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
      }
      WinHttpCloseHandle(hS);
    }

    auto* heap = new UpdateDownloadResult();
    if (okDl) {
      heap->ok = true;
      heap->folder = dir;
      heap->message = L"Downloaded update zip to:\r\n" + outFile;
    } else {
      heap->ok = false;
      heap->message = L"Direct zip download failed - open releases page instead.";
      heap->folder = useUrl;
    }
    PostMessageW(hMain, WM_APP_DL_UPDATE, heap->ok ? 1 : 0, (LPARAM)heap);
  }).detach();
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
  bool enabled;
};

static BtnStyle GetBtnStyle(HWND hwnd) {
  BtnStyle s{};
  s.primary = (GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0);
  s.enabled = IsWindowEnabled(hwnd) != FALSE;
  POINT pt; GetCursorPos(&pt);
  RECT rc; GetWindowRect(hwnd, &rc);
  s.hover = s.enabled && PtInRect(&rc, pt) != 0;
  s.pressed = s.enabled && (GetCapture() == hwnd) && s.hover;
  return s;
}

static LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc; GetClientRect(hwnd, &rc);

      HDC mem = CreateCompatibleDC(hdc);
      HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
      HGDIOBJ oldBmp = SelectObject(mem, bmp);

      // Corners sit on a card, so match that fill before the rounded face.
      HBRUSH cardBr = CreateSolidBrush(COL_CARD);
      FillRect(mem, &rc, cardBr);
      DeleteObject(cardBr);

      BtnStyle st = GetBtnStyle(hwnd);
      COLORREF fill, border, text;
      if (!st.enabled) {
        fill = RGB(22, 30, 46);
        border = RGB(38, 50, 68);
        text = RGB(100, 116, 139);
      } else if (st.primary) {
        fill = st.pressed ? COL_PRIMARY_DD : (st.hover ? COL_PRIMARY_D : COL_PRIMARY);
        border = fill;
        text = COL_ON_PRIMARY;
      } else {
        fill = st.pressed ? RGB(28, 40, 60) : (st.hover ? RGB(26, 38, 58) : COL_CARD);
        border = st.hover ? COL_PRIMARY : COL_BORDER;
        text = st.hover ? COL_TEXT : COL_TEXT_BODY;
      }

      int radius = 8;
      HBRUSH br = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, border);
      HGDIOBJ oldBr = SelectObject(mem, br);
      HGDIOBJ oldPen = SelectObject(mem, pen);
      RoundRect(mem, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
      SelectObject(mem, oldBr);
      SelectObject(mem, oldPen);
      DeleteObject(br);
      DeleteObject(pen);

      wchar_t caption[128];
      GetWindowTextW(hwnd, caption, 128);
      SetBkMode(mem, TRANSPARENT);
      SetTextColor(mem, text);
      SelectObject(mem, hFontBtn ? hFontBtn : hFontUi);
      DrawTextW(mem, caption, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

      BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, oldBmp);
      DeleteObject(bmp);
      DeleteDC(mem);
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
    case WM_SETCURSOR:
      if (IsWindowEnabled(hwnd)) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
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
    case WM_ENABLE:
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Recolour the 1px system border on edits and lists. Scrollbars stay themed.
static LRESULT CALLBACK FieldFrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR, DWORD_PTR) {
  if (msg == WM_NCPAINT || msg == WM_NCACTIVATE) {
    LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
    HDC hdc = GetWindowDC(hwnd);
    if (hdc) {
      RECT rc{};
      GetWindowRect(hwnd, &rc);
      OffsetRect(&rc, -rc.left, -rc.top);
      bool hot = (GetFocus() == hwnd);
      HBRUSH edge = CreateSolidBrush(hot ? COL_PRIMARY : COL_FIELD_LINE);
      FrameRect(hdc, &rc, edge);
      DeleteObject(edge);
      ReleaseDC(hwnd, hdc);
    }
    return lr;
  }
  if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
    LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    return lr;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Closed combo face. The drop-down list is a separate window and keeps working.
static LRESULT CALLBACK ComboProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc; GetClientRect(hwnd, &rc);
      int itemH = (int)SendMessageW(hwnd, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
      if (itemH < 16) itemH = rc.bottom;
      if (itemH > rc.bottom) itemH = rc.bottom;
      RECT vis{ 0, 0, rc.right, itemH };

      HBRUSH br = CreateSolidBrush(COL_BG_DEEP);
      FillRect(hdc, &vis, br);
      DeleteObject(br);
      bool hot = (GetFocus() == hwnd);
      HBRUSH edge = CreateSolidBrush(hot ? COL_PRIMARY : COL_FIELD_LINE);
      FrameRect(hdc, &vis, edge);
      DeleteObject(edge);

      wchar_t buf[260] = L"";
      int sel = (int)SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
      if (sel >= 0)
        SendMessageW(hwnd, CB_GETLBTEXT, sel, (LPARAM)buf);
      RECT tr = vis;
      tr.left += 10;
      tr.right -= 26;
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, COL_TEXT);
      SelectObject(hdc, hFontUi);
      DrawTextW(hdc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

      int cx = vis.right - 14;
      int cy = (vis.top + vis.bottom) / 2;
      HPEN pen = CreatePen(PS_SOLID, 2, hot ? COL_PRIMARY : COL_TEXT_DIM);
      HGDIOBJ oldPen = SelectObject(hdc, pen);
      MoveToEx(hdc, cx - 4, cy - 2, nullptr);
      LineTo(hdc, cx, cy + 3);
      LineTo(hdc, cx + 4, cy - 2);
      SelectObject(hdc, oldPen);
      DeleteObject(pen);

      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_CTLCOLORLISTBOX: {
      HDC ldc = (HDC)wParam;
      SetTextColor(ldc, COL_TEXT);
      SetBkColor(ldc, COL_BG_DEEP);
      return (LRESULT)hBrushInput;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_COMMAND:
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK CheckProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    HBRUSH br = CreateSolidBrush(COL_CARD);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    POINT pt; GetCursorPos(&pt);
    RECT wr; GetWindowRect(hwnd, &wr);
    bool hover = PtInRect(&wr, pt) != 0;

    const int box = 15;
    int y = (rc.bottom - box) / 2;
    if (y < 0) y = 0;
    RECT boxRc{ 1, y, 1 + box, y + box };
    if (checked) {
      HBRUSH fb = CreateSolidBrush(COL_PRIMARY);
      HPEN fp = CreatePen(PS_SOLID, 1, COL_PRIMARY);
      HGDIOBJ ob = SelectObject(hdc, fb);
      HGDIOBJ op = SelectObject(hdc, fp);
      RoundRect(hdc, boxRc.left, boxRc.top, boxRc.right, boxRc.bottom, 4, 4);
      SelectObject(hdc, ob);
      SelectObject(hdc, op);
      DeleteObject(fb);
      DeleteObject(fp);
      HPEN mark = CreatePen(PS_SOLID, 2, COL_ON_PRIMARY);
      HGDIOBJ om = SelectObject(hdc, mark);
      int x0 = boxRc.left;
      int y0 = boxRc.top;
      MoveToEx(hdc, x0 + 3, y0 + 8, nullptr);
      LineTo(hdc, x0 + 6, y0 + 11);
      LineTo(hdc, x0 + 12, y0 + 4);
      SelectObject(hdc, om);
      DeleteObject(mark);
    } else {
      HBRUSH fb = CreateSolidBrush(COL_BG_DEEP);
      HPEN fp = CreatePen(PS_SOLID, 1, hover ? COL_PRIMARY : COL_FIELD_LINE);
      HGDIOBJ ob = SelectObject(hdc, fb);
      HGDIOBJ op = SelectObject(hdc, fp);
      RoundRect(hdc, boxRc.left, boxRc.top, boxRc.right, boxRc.bottom, 4, 4);
      SelectObject(hdc, ob);
      SelectObject(hdc, op);
      DeleteObject(fb);
      DeleteObject(fp);
    }

    wchar_t cap[160] = L"";
    GetWindowTextW(hwnd, cap, 160);
    const wchar_t* shown = cap;
    while (*shown == L' ') ++shown;
    if (shown[0]) {
      RECT tr{ box + 8, 0, rc.right, rc.bottom };
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, hover ? COL_TEXT : COL_TEXT_BODY);
      SelectObject(hdc, hFontSmall);
      DrawTextW(hdc, shown, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
    TrackMouseEvent(&tme);
    InvalidateRect(hwnd, nullptr, FALSE);
  } else if (msg == WM_MOUSELEAVE || msg == BM_SETCHECK || msg == WM_LBUTTONUP) {
    LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
    InvalidateRect(hwnd, nullptr, FALSE);
    return lr;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void AllowDarkWindow(HWND hwnd);

static void StyleEdit(HWND h, bool multiline) {
  if (!h) return;
  SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
  // Dark scrollbars on the log; single-line fields have no scrollbar.
  AllowDarkWindow(h);
  SetWindowTheme(h, multiline ? L"DarkMode_Explorer" : L"", L"");
  SetWindowSubclass(h, FieldFrameProc, 2, 0);
}

static void StyleCombo(HWND h) {
  if (!h) return;
  SetWindowTheme(h, L"", L"");
  SendMessageW(h, CB_SETITEMHEIGHT, (WPARAM)-1, 26);
  SendMessageW(h, CB_SETITEMHEIGHT, 0, 24);
  SetWindowSubclass(h, ComboProc, 3, 0);
}

static void StyleCheck(HWND h) {
  if (!h) return;
  SetWindowTheme(h, L"", L"");
  SetWindowSubclass(h, CheckProc, 4, 0);
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
    SetBkColor(hdc, COL_CARD);
    return hBrushCard ? hBrushCard : hBrushBg;
  }
  // Field labels + Remember me sit on painted cards (COL_CARD)
  SetTextColor(hdc, COL_TEXT_BODY);
  SetBkColor(hdc, COL_CARD);
  SetBkMode(hdc, TRANSPARENT);
  return hBrushCard ? hBrushCard : hBrushBg;
}

// Checkboxes without text (we paint labels as STATIC). Keep box bg on card colour.
static HBRUSH OnCtlColorBtn(HDC hdc) {
  SetTextColor(hdc, COL_TEXT_DIM); // same as field labels
  SetBkColor(hdc, COL_CARD);
  SetBkMode(hdc, OPAQUE);
  return hBrushCard ? hBrushCard : hBrushBg;
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
  SetStatus(L"Running speed test (sample of 100mb.bin through your proxy)...", COL_PRIMARY);
  AppendResult(L"--- Speed test ---");

  std::thread([cfg]() {
    SpeedTestResult r;
    try {
      r = SpeedTestProxy(cfg);
    } catch (...) {
      r.success = false;
      r.message = L"Speed test failed (internal error).";
    }
    auto* heap = new SpeedTestResult(std::move(r));
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

// Short rounded section marker. Height matches the section label, not the card.
static void PaintAccentBar(HDC hdc, int x, int y, int w, int h, COLORREF color) {
  if (w < 1 || h < 1) return;
  HBRUSH br = CreateSolidBrush(color);
  HPEN pen = CreatePen(PS_SOLID, 1, color);
  HGDIOBJ oldBr = SelectObject(hdc, br);
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  int r = (w < h ? w : h);
  RoundRect(hdc, x, y, x + w, y + h, r, r);
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

static void DrawTextBox(HDC hdc, const wchar_t* text, RECT rc, COLORREF color, HFONT font, UINT flags) {
  SelectObject(hdc, font);
  SetTextColor(hdc, color);
  SetBkMode(hdc, TRANSPARENT);
  DrawTextW(hdc, text, -1, &rc, flags | DT_NOPREFIX);
}

static void FillGradientV(HDC hdc, RECT rc, COLORREF top, COLORREF bot) {
  if (rc.right <= rc.left || rc.bottom <= rc.top) return;
  TRIVERTEX v[2]{};
  v[0].x = rc.left;
  v[0].y = rc.top;
  v[0].Red = (COLOR16)(GetRValue(top) << 8);
  v[0].Green = (COLOR16)(GetGValue(top) << 8);
  v[0].Blue = (COLOR16)(GetBValue(top) << 8);
  v[1].x = rc.right;
  v[1].y = rc.bottom;
  v[1].Red = (COLOR16)(GetRValue(bot) << 8);
  v[1].Green = (COLOR16)(GetGValue(bot) << 8);
  v[1].Blue = (COLOR16)(GetBValue(bot) << 8);
  GRADIENT_RECT g{ 0, 1 };
  GradientFill(hdc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
}

static void PaintHeader(HDC hdc, int width) {
  RECT top{ 0, 0, width, 100 };
  FillGradientV(hdc, top, RGB(14, 28, 46), COL_BG);

  RECT bar{ 0, 0, width, 2 };
  HBRUSH brBar = CreateSolidBrush(COL_PRIMARY);
  FillRect(hdc, &bar, brBar);
  DeleteObject(brBar);

  const int iconSz = HEADER_ICON_PX;
  const int iconX = PAD;
  const int iconY = 18;
  if (g_headerIcon) {
    DrawIconEx(hdc, iconX, iconY, g_headerIcon, iconSz, iconSz, 0, nullptr, DI_NORMAL);
  } else {
    RECT mark{ iconX, iconY, iconX + iconSz, iconY + iconSz };
    FillRoundRect(hdc, mark, 12, COL_PRIMARY, COL_PRIMARY);
    DrawTextBox(hdc, L"Pi", mark, COL_ON_PRIMARY, hFontBtn ? hFontBtn : hFontUi,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  int textX = iconX + iconSz + 14;
  SelectObject(hdc, hFontTitle);
  SIZE brandSz{};
  GetTextExtentPoint32W(hdc, L"ProxyPi", 7, &brandSz);
  DrawTextAt(hdc, L"ProxyPi", textX, 20, brandSz.cx + 4, COL_TEXT, hFontTitle);
  DrawTextAt(hdc, L"Tester", textX + brandSz.cx + 8, 20, 140, COL_PRIMARY, hFontTitle);
  DrawTextAt(hdc, L"UK residential proxy test and app routing", textX, 52, 460, COL_TEXT_DIM, hFontSmall);

  int right = width - PAD;
  if (right > textX + 220) {
    std::wstring ver = L"v";
    ver += APP_VERSION;
    DrawTextAt(hdc, ver.c_str(), right - 140, 28, 140, COL_TEXT, hFontSmall, DT_RIGHT);
    DrawTextAt(hdc, L"proxypi.co.uk", right - 160, 50, 160, COL_TEXT_DIM, hFontSmall, DT_RIGHT);
  }

  RECT rule{ 0, 99, width, 100 };
  HBRUSH ruleBr = CreateSolidBrush(COL_BORDER);
  FillRect(hdc, &rule, ruleBr);
  DeleteObject(ruleBr);
}

static void PaintMetricCards(HDC hdc, int y) {
  int cardH = g_lay.metricsH > 0 ? g_lay.metricsH : 72;
  int gap = 10;
  int w3 = (g_lay.cardW - gap * 2) / 3;
  if (w3 < 40) w3 = 40;

  auto paintOne = [&](int x, const wchar_t* label, const std::wstring& value, COLORREF valueColor) {
    RECT rc{ x, y, x + w3, y + cardH };
    FillRoundRect(hdc, rc, 14, COL_CARD, COL_CARD_LINE);
    COLORREF accentCol = valueColor == COL_TEXT_DIM ? COL_BORDER : valueColor;
    PaintAccentBar(hdc, x + 16, y + 16, 4, 28, accentCol);
    DrawTextAt(hdc, label, x + 28, y + 8, w3 - 40, COL_TEXT_DIM, hFontSection ? hFontSection : hFontSmall);
    DrawTextAt(hdc, value.c_str(), x + 28, y + (cardH >= 64 ? 30 : 26), w3 - 40, valueColor,
               hFontMetric ? hFontMetric : hFontUi, DT_LEFT | DT_END_ELLIPSIS);
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
  if (rc.bottom <= rc.top + 8 || rc.right <= rc.left + 8) return;
  FillRoundRect(hdc, rc, 16, COL_CARD, COL_CARD_LINE);
  // Marker matches the section-label cap and stays inside the corner radius.
  const int titleTop = rc.top + 8;
  const int titleH = 20;
  const int barH = 14;
  const int barW = 4;
  const int barX = rc.left + 18;
  const int barY = titleTop + (titleH - barH) / 2;
  PaintAccentBar(hdc, barX, barY, barW, barH, COL_PRIMARY);
  RECT titleRc{ barX + barW + 8, titleTop, rc.right - 16, titleTop + titleH };
  DrawTextBox(hdc, title, titleRc, COL_TEXT_BODY, hFontSection ? hFontSection : hFontSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Place control at document (x,y); converts to screen using g_lay.scrollY
static void MoveCtrl(HWND h, int x, int y, int w, int hh) {
  if (h && IsWindow(h))
    SetWindowPos(h, nullptr, x, y - g_lay.scrollY + MENU_STRIP_H, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void HideCtrl(HWND h) {
  if (h && IsWindow(h)) ShowWindow(h, SW_HIDE);
}

static int MaxScrollY() {
  int m = g_lay.contentH - g_lay.clientH;
  return (m > 0) ? m : 0;
}

static void UpdateScrollBar(HWND hwnd) {
  const int maxY = MaxScrollY();
  if (g_lay.scrollY > maxY) g_lay.scrollY = maxY;
  if (g_lay.scrollY < 0) g_lay.scrollY = 0;
  g_lay.needScroll = (maxY > 0);

  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = (g_lay.contentH > 0) ? (g_lay.contentH - 1) : 0;
  si.nPage = (UINT)((g_lay.clientH > 0) ? g_lay.clientH : 1);
  si.nPos = g_lay.scrollY;
  SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  ShowScrollBar(hwnd, SB_VERT, g_lay.needScroll ? TRUE : FALSE);
}

// Scroll without full re-layout (moves children + invalidates)
static void SetScrollY(HWND hwnd, int newY) {
  int maxY = MaxScrollY();
  if (newY < 0) newY = 0;
  if (newY > maxY) newY = maxY;
  if (newY == g_lay.scrollY) return;
  int dy = g_lay.scrollY - newY; // positive = content moves down on screen
  g_lay.scrollY = newY;
  RECT clip{ 0, MENU_STRIP_H, g_lay.clientW, MENU_STRIP_H + g_lay.clientH };
  ScrollWindowEx(hwnd, 0, dy, nullptr, &clip, nullptr, nullptr,
                 SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_POS;
  si.nPos = g_lay.scrollY;
  SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  UpdateWindow(hwnd);
}

// Flow-layout packer: place left-to-right, wrap when out of width.
// Tracks max control height per row so Bottom() is accurate after mixed heights.
// Y coordinates are document-space (MoveCtrl applies scroll).
struct FlowPack {
  int left = 0, right = 0, x = 0, y = 0;
  int rowH = 30, gap = 6;
  int rowMaxH = 30;
  int lineGap = 6;
  void Begin(int L, int R, int Y, int rowHeight = 30, int g = 6) {
    left = L; right = R; x = L; y = Y;
    rowH = rowHeight; gap = g; rowMaxH = rowHeight; lineGap = 6;
  }
  void Place(HWND h, int w, int hh) {
    if (!h) return;
    if (x > left && x + w > right) {
      y += rowMaxH + lineGap;
      x = left;
      rowMaxH = rowH;
    }
    if (w > right - left) w = right - left;
    if (w < 16) w = 16;
    MoveCtrl(h, x, y, w, hh);
    if (hh > rowMaxH && hh <= rowH + 8) rowMaxH = hh;
    else if (hh <= 40 && hh > rowMaxH) rowMaxH = hh;
    if (hh > 40 && rowMaxH < rowH) rowMaxH = rowH;
    x += w + gap;
  }
  void NewRow() {
    if (x != left) {
      y += rowMaxH + lineGap;
      x = left;
      rowMaxH = rowH;
    }
  }
  int Bottom() const { return y + rowMaxH; }
};

// Single top-to-bottom layout in document space. When the viewport is shorter
// than the comfortable content height, a vertical scrollbar appears instead of
// squashing the top half.
static void ApplyLayout(HWND hwnd) {
  if (g_inLayout) return;
  g_inLayout = true;

  // Up to 2 passes: scrollbar appearing/disappearing changes client width
  for (int pass = 0; pass < 2; ++pass) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top - MENU_STRIP_H;
    if (cw < 100) cw = 100;
    if (ch < 100) ch = 100;

    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);

    const int outerPad = (cw < 760) ? 12 : 20;
    const int gap = 16;
    const int headerEnd = 116;
    const int footerBand = 72; // status + branding (document space)
    const int labelW = 72;
    const int rowH = 28;
    const int gapY = 32;

    g_lay.clientW = cw;
    g_lay.clientH = ch;
    g_lay.cardX = outerPad;
    g_lay.cardW = cw - outerPad * 2;
    if (g_lay.cardW < 200) g_lay.cardW = 200;
    g_lay.sideBySide = (cw >= SIDE_BY_SIDE_MIN_W);
    g_lay.narrow = (g_lay.cardW < 720);
    const bool narrow = g_lay.narrow;

    // Always use comfortable sizes — never squash top half for short windows
    const int acctCardH = 206;
    const int cfgCardH  = 232;
    g_lay.metricsH = 72;

    // ---- Account + Config cards ----
    if (g_lay.sideBySide) {
      int half = (g_lay.cardW - gap) / 2;
      g_lay.acctLeft = g_lay.cardX;
      g_lay.acctW = half;
      g_lay.cfgLeft = g_lay.cardX + half + gap;
      g_lay.cfgW = half;
      g_lay.acctTop = headerEnd;
      g_lay.cfgTop = g_lay.acctTop;
      int sharedH = (cfgCardH > acctCardH) ? cfgCardH : acctCardH;
      g_lay.acctBot = g_lay.acctTop + sharedH;
      g_lay.cfgBot = g_lay.acctBot;
    } else {
      g_lay.acctLeft = g_lay.cardX;
      g_lay.acctW = g_lay.cardW;
      g_lay.cfgLeft = g_lay.cardX;
      g_lay.cfgW = g_lay.cardW;
      g_lay.acctTop = headerEnd;
      g_lay.acctBot = g_lay.acctTop + acctCardH;
      g_lay.cfgTop = g_lay.acctBot + gap;
      g_lay.cfgBot = g_lay.cfgTop + cfgCardH;
    }

    auto layoutFields = [&](int left, int top, int cardW, bool account) {
      int pad = 16;
      int labelX = left + pad;
      int fieldX = left + pad + labelW;
      int fieldW = cardW - pad * 2 - labelW;
      if (fieldW < 80) fieldW = 80;
      int y = top + 36;
      if (account) {
        MoveCtrl(hLblAcctEmail, labelX, y + 4, labelW, 18);
        MoveCtrl(hAcctEmail, fieldX, y, fieldW, rowH);
        y += gapY;
        MoveCtrl(hLblAcctPass, labelX, y + 4, labelW, 18);
        MoveCtrl(hAcctPass, fieldX, y, fieldW, rowH);
        y += gapY;
        MoveCtrl(hCheckRemember, left + pad, y + 2, 20, 20);
        MoveCtrl(hLblRemember, left + pad + 24, y + 4, 160, 18);
        y += 26;
        int loginW = cardW - pad * 2;
        if (loginW > BTN_LOGIN_W + 40) loginW = BTN_LOGIN_W + 40;
        if (loginW < 140) loginW = cardW - pad * 2;
        MoveCtrl(hBtnLogin, left + pad, y, loginW, 32);
      } else {
        MoveCtrl(hLblType, labelX, y + 4, labelW, 18);
        int typeW = fieldW > 150 ? 150 : fieldW;
        MoveCtrl(hComboType, fieldX, y, typeW, 200);
        y += gapY;
        MoveCtrl(hLblHost, labelX, y + 4, labelW, 18);
        MoveCtrl(hHost, fieldX, y, fieldW, rowH);
        y += gapY;
        MoveCtrl(hLblPort, labelX, y + 4, labelW, 18);
        MoveCtrl(hPort, fieldX, y, 84, rowH);
        int authNeed = 130;
        if (fieldX + 94 + authNeed < left + cardW - pad) {
          MoveCtrl(hCheckAuth, fieldX + 94, y + 3, authNeed, 22);
        } else {
          y += gapY;
          MoveCtrl(hCheckAuth, fieldX, y + 3, authNeed, 22);
        }
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

    int yCursor = (g_lay.acctBot > g_lay.cfgBot ? g_lay.acctBot : g_lay.cfgBot) + gap;

    // ---- Metrics ----
    g_lay.metricsY = yCursor;
    yCursor = g_lay.metricsY + g_lay.metricsH + 10;

    // ---- Primary + secondary buttons ----
    int btnH = 38;
    int secH = 28;
    int primaryW = narrow ? 112 : BTN_PRIMARY_W;
    int secondaryW = narrow ? 84 : BTN_SECONDARY_W;

    FlowPack fp;
    fp.Begin(g_lay.cardX, g_lay.cardX + g_lay.cardW, yCursor, btnH, 8);
    g_lay.testBtnY = yCursor;
    fp.Place(hBtnTest, primaryW, btnH);
    fp.Place(hBtnSpeed, primaryW, btnH);
    yCursor = fp.Bottom() + 6;

    fp.Begin(g_lay.cardX, g_lay.cardX + g_lay.cardW, yCursor, secH, 6);
    g_lay.secBtnY = yCursor;
    fp.Place(hBtnSave, secondaryW, secH);
    fp.Place(hBtnLoad, secondaryW, secH);
    fp.Place(hBtnSite, secondaryW, secH);
    yCursor = fp.Bottom() + gap;

    HideCtrl(hBtnDevChrome);
    HideCtrl(hBtnDevEdge);
    HideCtrl(hBtnDevFirefox);
    HideCtrl(hBtnDevRuneLite);
    HideCtrl(hBtnDevJagex);

    // ---- ACTIVE APPS: natural toolbar, then list/results ----
    g_lay.routeTop = yCursor;
    const int rx = g_lay.cardX + 16;
    const int rRight = g_lay.cardX + g_lay.cardW - 16;
    const int rw = rRight - rx;
    const int gapCards = gap;

    MoveCtrl(hChromePathLabel, rx, g_lay.routeTop + 24, rw, 16);

    FlowPack tools;
    tools.Begin(rx, rRight, g_lay.routeTop + 44, 28, 6);
    int comboW = narrow ? 140 : COMBO_APPS_W;
    if (comboW > rw - 80) comboW = (rw > 200) ? rw / 2 : rw - 80;
    if (comboW < 100) comboW = 100;
    tools.Place(hComboApps, comboW, 200);
    tools.Place(hBtnLaunchApp, narrow ? 84 : BTN_LAUNCH_W, 28);
    tools.Place(hBtnBrowseApp, 64, 28);
    tools.Place(hBtnSessRefresh, 64, 28);
    tools.Place(hBtnQuitApp, 64, 28);
    tools.Place(hBtnVerifyIp, 80, 28);

    tools.NewRow();
    int urlBtnW = (narrow ? 84 : BTN_URL_W);
    int copyW = (narrow ? 84 : BTN_COPY_W);
    int reportW = narrow ? 72 : 88;
    int urlBtns = urlBtnW + copyW + reportW + tools.gap * 3;
    int urlEditW = (rRight - tools.x) - urlBtns;
    if (urlEditW < 72) {
      tools.NewRow();
      urlEditW = rw - urlBtns;
    }
    if (urlEditW < 60) urlEditW = 60;
    tools.Place(hEditOpenUrl, urlEditW, 26);
    tools.Place(hBtnOpenUrl, urlBtnW, 28);
    tools.Place(hBtnCopyIp, copyW, 28);
    tools.Place(hBtnCopyReport, reportW, 28);

    tools.NewRow();
    tools.Place(hComboProfile, narrow ? 110 : 150, 200);
    tools.Place(hBtnSaveProfile, 64, 26);
    tools.Place(hBtnRunProfile, 64, 26);
    tools.Place(hBtnDelProfile, 64, 26);
    tools.Place(hCheckTray, narrow ? 120 : 148, 22);

    int sessTop = tools.Bottom() + 8;
    g_lay.routeToolbarH = sessTop - g_lay.routeTop;

    // Comfortable content height with preferred list/results (no squash)
    const int contentAtPref =
        sessTop + PREF_LIST_H + gapCards + PREF_RES_H + footerBand;

    int listH, resH;
    if (ch >= contentAtPref) {
      // Viewport tall enough: expand list/results to fill it (no scrollbar)
      int pool = ch - footerBand - sessTop - gapCards;
      if (pool < PREF_LIST_H + PREF_RES_H) pool = PREF_LIST_H + PREF_RES_H;
      listH = (int)(pool * 0.58);
      if (listH < PREF_LIST_H) listH = PREF_LIST_H;
      resH = pool - listH;
      if (resH < PREF_RES_H) {
        resH = PREF_RES_H;
        listH = pool - resH;
      }
    } else {
      // Short viewport: keep natural sizes and enable scrolling
      listH = PREF_LIST_H;
      resH = PREF_RES_H;
    }

    g_lay.routeBot = sessTop + listH;
    g_lay.resTop = g_lay.routeBot + gapCards;
    g_lay.resBot = g_lay.resTop + resH;

    g_lay.statusY = g_lay.resBot + 10;
    g_lay.footerY = g_lay.statusY + 48;
    g_lay.contentH = g_lay.footerY + 22;
    // When expanded to fill viewport, content matches client (no scroll)
    if (ch >= contentAtPref && g_lay.contentH < ch)
      g_lay.contentH = ch;

    // Clamp scroll before placing controls
    int maxY = g_lay.contentH - ch;
    if (maxY < 0) maxY = 0;
    if (g_lay.scrollY > maxY) g_lay.scrollY = maxY;
    if (g_lay.scrollY < 0) g_lay.scrollY = 0;

    int sessH = g_lay.routeBot - sessTop - 16;
    if (sessH < 24) sessH = 24;
    MoveCtrl(hSessionList, rx, sessTop, rw, sessH);

    int resInnerTop = g_lay.resTop + 32;
    int resInnerH = g_lay.resBot - resInnerTop - 16;
    if (resInnerH < 20) resInnerH = 20;
    MoveCtrl(hResult, rx, resInnerTop, rw, resInnerH);

    MoveCtrl(hStatus, g_lay.cardX + 28, g_lay.statusY + 6, g_lay.cardW - 40, 26);

    UpdateScrollBar(hwnd);

    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);

    // If scrollbar toggle changed client size, re-measure once
    RECT rc2{};
    GetClientRect(hwnd, &rc2);
    if (rc2.right - rc2.left == cw && rc2.bottom - rc2.top == ch + MENU_STRIP_H)
      break;
  }

  RedrawWindow(hwnd, nullptr, nullptr,
               RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
  g_inLayout = false;
}

// ---- window proc ----
static LRESULT HandleMenuBarPaint(HWND hwnd, UINT msg, LPARAM lParam);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case 0x0091: // WM_UAHDRAWMENU
    case 0x0092: // WM_UAHDRAWMENUITEM
      return HandleMenuBarPaint(hwnd, msg, lParam);

    case WM_CREATE: {
      hMain = hwnd;

      // Negative height is the character size in pixels. Segoe UI is on every supported Windows.
      hFontTitle = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontUi = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontBtn = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontSection = CreateFontW(-11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontMetric = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      hFontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
      {
        HDC dc = GetDC(hwnd);
        HFONT prev = (HFONT)SelectObject(dc, hFontMono);
        wchar_t face[64] = L"";
        GetTextFaceW(dc, 64, face);
        SelectObject(dc, prev);
        ReleaseDC(hwnd, dc);
        if (_wcsicmp(face, L"Cascadia Mono") != 0) {
          DeleteObject(hFontMono);
          hFontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        }
      }

      hBrushBg = CreateSolidBrush(COL_BG);
      hBrushCard = CreateSolidBrush(COL_CARD);
      hBrushInput = CreateSolidBrush(COL_BG_DEEP);
      hBrushPrimary = CreateSolidBrush(COL_PRIMARY);
      if (!g_headerIcon) {
        g_headerIcon = (HICON)LoadImageW((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE),
          MAKEINTRESOURCEW(101), IMAGE_ICON, HEADER_ICON_PX, HEADER_ICON_PX, LR_DEFAULTCOLOR);
      }

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
      StyleEdit(hAcctEmail, false);
      SendMessageW(hAcctEmail, EM_SETCUEBANNER, TRUE, (LPARAM)L"you@email.com");

      hLblAcctPass = mkLabel(L"Password");
      hAcctPass = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_ACCT_PASS, nullptr, nullptr);
      SendMessageW(hAcctPass, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      StyleEdit(hAcctPass, false);

      // Empty text on checkbox; label is a STATIC so colour matches Email/Password
      hCheckRemember = CreateWindowW(L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 20, 20, hwnd, (HMENU)ID_CHECK_REMEMBER, nullptr, nullptr);
      StyleCheck(hCheckRemember);
      hLblRemember = CreateWindowW(L"STATIC", L"Remember me",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 160, 18, hwnd, nullptr, nullptr, nullptr);
      SendMessageW(hLblRemember, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

      // Note: do not use single '&' in labels - Win32 treats it as accelerator
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
      StyleCombo(hComboType);

      hLblHost = mkLabel(L"Host");
      hHost = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_HOST, nullptr, nullptr);
      SendMessageW(hHost, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      StyleEdit(hHost, false);
      SendMessageW(hHost, EM_SETCUEBANNER, TRUE, (LPARAM)L"proxy host");

      hLblPort = mkLabel(L"Port");
      hPort = CreateWindowW(L"EDIT", L"18721",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_PORT, nullptr, nullptr);
      SendMessageW(hPort, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      StyleEdit(hPort, false);

      hCheckAuth = CreateWindowW(L"BUTTON", L" Authentication",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 160, 24, hwnd, (HMENU)ID_CHECK_AUTH, nullptr, nullptr);
      StyleCheck(hCheckAuth);
      SendMessageW(hCheckAuth, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
      SendMessageW(hCheckAuth, BM_SETCHECK, BST_CHECKED, 0);

      hLblUser = mkLabel(L"Username");
      hUser = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_USER, nullptr, nullptr);
      SendMessageW(hUser, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      StyleEdit(hUser, false);
      SendMessageW(hUser, EM_SETCUEBANNER, TRUE, (LPARAM)L"proxy username");

      hLblPass = mkLabel(L"Password");
      hPass = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        0, 0, 100, 32, hwnd, (HMENU)ID_EDIT_PASS, nullptr, nullptr);
      SendMessageW(hPass, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      StyleEdit(hPass, false);

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
      StyleCombo(hComboApps);
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
      StyleEdit(hEditOpenUrl, false);
      hBtnOpenUrl = CreateStyledButton(hwnd, L"Open URL", 0, 0, BTN_URL_W, 32, ID_BTN_OPEN_URL, true);
      hBtnCopyIp = CreateStyledButton(hwnd, L"Copy IP", 0, 0, BTN_COPY_W, 32, ID_BTN_COPY_IP, false);
      hBtnCopyReport = CreateStyledButton(hwnd, L"Report", 0, 0, 88, 30, ID_BTN_COPY_REPORT, false);
      hBtnQuitApp = CreateStyledButton(hwnd, L"Quit app", 0, 0, 72, 30, ID_BTN_QUIT_APP, false);
      hBtnVerifyIp = CreateStyledButton(hwnd, L"Verify IP", 0, 0, 88, 30, ID_BTN_VERIFY_IP, true);

      hComboProfile = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 160, 200, hwnd, (HMENU)ID_COMBO_PROFILE, nullptr, nullptr);
      SendMessageW(hComboProfile, WM_SETFONT, (WPARAM)hFontUi, TRUE);
      StyleCombo(hComboProfile);
      hBtnSaveProfile = CreateStyledButton(hwnd, L"Save", 0, 0, 70, 28, ID_BTN_SAVE_PROFILE, false);
      hBtnRunProfile = CreateStyledButton(hwnd, L"Run", 0, 0, 70, 28, ID_BTN_RUN_PROFILE, true);
      hBtnDelProfile = CreateStyledButton(hwnd, L"Delete", 0, 0, 70, 28, ID_BTN_DEL_PROFILE, false);
      hCheckTray = CreateWindowW(L"BUTTON", L" Minimize to tray",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 160, 22, hwnd, (HMENU)ID_CHECK_TRAY, nullptr, nullptr);
      StyleCheck(hCheckTray);
      SendMessageW(hCheckTray, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
      // Default unchecked — tray hide looks like a crash to many users
      SendMessageW(hCheckTray, BM_SETCHECK, BST_UNCHECKED, 0);

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
      AllowDarkWindow(hSessionList);
      SetWindowTheme(hSessionList, L"DarkMode_Explorer", nullptr);
      SetWindowSubclass(hSessionList, FieldFrameProc, 2, 0);

      hResult = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 100, 60, hwnd, (HMENU)ID_EDIT_RESULT, nullptr, nullptr);
      SendMessageW(hResult, WM_SETFONT, (WPARAM)hFontMono, TRUE);
      StyleEdit(hResult, true);

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
        SetPortPairAndRefresh(g_socksPort, g_httpPort);
        SetStatus(L"Loaded saved settings.", COL_TEXT_DIM);
      } else {
        cfg.type = ProxyType::SOCKS5;
        cfg.host = L"";
        cfg.port = 18721;
        cfg.useAuth = true;
        WriteConfigToUI(cfg);
        LoadAccountCredentials(); // may still have remembered account email/pass
        SetStatus(L"Ready - enter your ProxyPi host, port and credentials.", COL_TEXT_DIM);
      }
      LoadAppRoutePrefs();
      RefreshProfileCombo();
      RefreshChromePathLabel();
      SetLogWindow(hResult);
      SetLogMainWindow(hwnd);

      AppendResult(L"ProxyPiTester " + std::wstring(APP_VERSION) + L" ready.");
      AppendResult(L"Log in, then Test / Verify IP / open apps. Profiles + tray enabled.");
      AppendResult(L"Tip: SOCKS5 + auth recommended. Fully quit Electron apps before re-launch.");
      RefreshSessionList();
      SetTimer(hwnd, TIMER_SESSIONS, 2000, nullptr);
      SetTimer(hwnd, TIMER_HEALTH, 5000, nullptr);
      // Quiet update check is started from wWinMain after ShowWindow (not here)
      if (hAcctEmail) SetFocus(hAcctEmail);
      StartupLog(L"WM_CREATE complete");
      return 0;
    }

    case WM_TIMER:
      if (wParam == TIMER_SESSIONS) {
        RefreshSessionList();
        RefreshChromePathLabel();
      } else if (wParam == TIMER_HEALTH) {
        // Bridge auto-restart (#20) + RuneLite wrap re-arm (#21)
        ProxyConfig tmp;
        if (GetLastBridgeConfig(tmp) && !IsLocalAuthBridgeRunning()) {
          std::wstring err;
          if (EnsureBridgeAlive(err)) {
            AppendResult(L"Bridge auto-restarted on 127.0.0.1:" +
                         std::to_wstring(GetLocalAuthBridgePort()));
            RefreshChromePathLabel();
          }
        }
        if (IsRuneLiteWrapDesired()) {
          std::wstring werr;
          int before = GetRuneLiteSocksWrapPort();
          ReArmRuneLiteWrapIfNeeded(werr);
          int after = GetRuneLiteSocksWrapPort();
          if (after > 0 && before > 0 && after != before)
            AppendResult(L"RuneLite wrap re-armed on port " + std::to_wstring(after));
          else if (after > 0 && before <= 0)
            AppendResult(L"RuneLite wrap restored on port " + std::to_wstring(after));
        }
      }
      return 0;

    case WM_SYSCOMMAND:
      if ((wParam & 0xFFF0) == SC_MINIMIZE && g_minimizeToTray) {
        TrayAdd(hwnd, true);
        ShowWindow(hwnd, SW_HIDE);
        StartupLog(L"minimized to tray (not a crash)");
        SetStatus(L"Minimized to tray — click the tray icon near the clock to restore.", COL_PRIMARY);
        return 0;
      }
      break;

    case WM_TRAYICON:
      if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
        TrayShowWindow(hwnd);
      } else if (lParam == WM_RBUTTONUP) {
        POINT pt; GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Show ProxyPiTester");
        AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit");
        SetForegroundWindow(hwnd);
        TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(m);
      }
      return 0;

    case WM_DPICHANGED: {
      g_dpi = HIWORD(wParam);
      RECT* prc = (RECT*)lParam;
      SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                   prc->right - prc->left, prc->bottom - prc->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      ApplyLayout(hwnd);
      return 0;
    }

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

      // Document origin sits just under the fixed command strip.
      SetWindowOrgEx(mem, 0, g_lay.scrollY - MENU_STRIP_H, nullptr);

      PaintHeader(mem, client.right);

      RECT acctCard{ g_lay.acctLeft, g_lay.acctTop, g_lay.acctLeft + g_lay.acctW, g_lay.acctBot };
      PaintCardChrome(mem, acctCard, L"PROXYPI ACCOUNT");

      RECT cfgCard{ g_lay.cfgLeft, g_lay.cfgTop, g_lay.cfgLeft + g_lay.cfgW, g_lay.cfgBot };
      PaintCardChrome(mem, cfgCard, L"PROXY CONFIGURATION");

      PaintMetricCards(mem, g_lay.metricsY);

      RECT routeCard{ g_lay.cardX, g_lay.routeTop, g_lay.cardX + g_lay.cardW, g_lay.routeBot };
      PaintCardChrome(mem, routeCard, L"ACTIVE APPS");

      RECT resCard{ g_lay.cardX, g_lay.resTop, g_lay.cardX + g_lay.cardW, g_lay.resBot };
      PaintCardChrome(mem, resCard, L"RESULTS");

      int statusTop = g_lay.statusY;
      if (statusTop < g_lay.resBot + 6) statusTop = g_lay.resBot + 6;
      RECT statusBg{ g_lay.cardX, statusTop, g_lay.cardX + g_lay.cardW, statusTop + 36 };
      FillRoundRect(mem, statusBg, 12, COL_CARD, COL_CARD_LINE);
      PaintAccentBar(mem, g_lay.cardX + 16, statusTop + 11, 4, 14, g_statusColor);

      {
        std::wstring foot = L"No logging  \u00B7  UK residential  \u00B7  Pay as you go  \u00B7  v";
        foot += APP_VERSION;
        RECT footRc{ 8, g_lay.footerY - 2, client.right - 8, g_lay.footerY + 18 };
        SelectObject(mem, hFontSmall);
        SetTextColor(mem, COL_TEXT_DIM);
        SetBkMode(mem, TRANSPARENT);
        DrawTextW(mem, foot.c_str(), -1, &footRc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      }

      SetWindowOrgEx(mem, 0, 0, nullptr);
      {
        RECT strip{ 0, 0, client.right, MENU_STRIP_H };
        HBRUSH sb = CreateSolidBrush(RGB(10, 15, 26));
        FillRect(mem, &strip, sb);
        DeleteObject(sb);
        RECT rule{ 0, MENU_STRIP_H - 1, client.right, MENU_STRIP_H };
        HBRUSH rb = CreateSolidBrush(COL_BORDER);
        FillRect(mem, &rule, rb);
        DeleteObject(rb);
        RECT fileRc{ 8, 2, 78, MENU_STRIP_H - 3 };
        RECT helpRc{ 78, 2, 152, MENU_STRIP_H - 3 };
        auto paintCmd = [&](RECT rc, const wchar_t* label, bool hot) {
          if (hot) {
            HBRUSH hb = CreateSolidBrush(RGB(24, 40, 62));
            FillRect(mem, &rc, hb);
            DeleteObject(hb);
          }
          DrawTextBox(mem, label, rc, hot ? COL_PRIMARY : COL_TEXT, hFontUi,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        paintCmd(fileRc, L"File", g_menuHot == 1);
        paintCmd(helpRc, L"Help", g_menuHot == 2);
      }
      BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_VSCROLL: {
      int y = g_lay.scrollY;
      const int line = 32;
      const int page = (g_lay.clientH > 80) ? (g_lay.clientH - 40) : 80;
      switch (LOWORD(wParam)) {
        case SB_LINEUP:        y -= line; break;
        case SB_LINEDOWN:      y += line; break;
        case SB_PAGEUP:        y -= page; break;
        case SB_PAGEDOWN:      y += page; break;
        case SB_TOP:           y = 0; break;
        case SB_BOTTOM:        y = MaxScrollY(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
          SCROLLINFO si{};
          si.cbSize = sizeof(si);
          si.fMask = SIF_TRACKPOS;
          GetScrollInfo(hwnd, SB_VERT, &si);
          y = si.nTrackPos;
          break;
        }
        default: break;
      }
      SetScrollY(hwnd, y);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      // If cursor is over Results or session list, let those controls scroll first
      POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      HWND hit = WindowFromPoint(pt);
      if (hit && (hit == hResult || hit == hSessionList ||
                  GetParent(hit) == hResult || GetParent(hit) == hSessionList)) {
        // Forward to the control
        SendMessageW(hit, WM_MOUSEWHEEL, wParam, lParam);
        return 0;
      }
      if (!g_lay.needScroll) return 0;
      int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      // 3 lines per notch
      int step = MulDiv(delta, 3 * 32, WHEEL_DELTA);
      SetScrollY(hwnd, g_lay.scrollY - step);
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
      return (LRESULT)OnCtlColorBtn((HDC)wParam);

    case WM_MEASUREITEM: {
      auto* ms = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
      if (ms->CtlType != ODT_MENU) break;
      const wchar_t* t = reinterpret_cast<const wchar_t*>(ms->itemData);
      if (!t || !t[0]) {
        ms->itemWidth = 200;
        ms->itemHeight = 9;
      } else {
        HDC hdc = GetDC(hwnd);
        HFONT old = (HFONT)SelectObject(hdc, hFontUi ? hFontUi : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        SIZE sz{};
        GetTextExtentPoint32W(hdc, t, (int)wcslen(t), &sz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        ms->itemWidth = sz.cx + 56;
        ms->itemHeight = 30;
      }
      return TRUE;
    }

    case WM_DRAWITEM: {
      auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (dis->CtlType == ODT_MENU) {
        const wchar_t* text = reinterpret_cast<const wchar_t*>(dis->itemData);
        bool sep = (!text || !text[0]);
        bool hot = !sep && (dis->itemState & ODS_SELECTED);
        HBRUSH br = CreateSolidBrush(hot ? RGB(24, 40, 62) : RGB(12, 18, 30));
        FillRect(dis->hDC, &dis->rcItem, br);
        DeleteObject(br);
        if (sep) {
          int mid = (dis->rcItem.top + dis->rcItem.bottom) / 2;
          RECT line{ dis->rcItem.left + 12, mid, dis->rcItem.right - 12, mid + 1 };
          HBRUSH lb = CreateSolidBrush(COL_BORDER);
          FillRect(dis->hDC, &line, lb);
          DeleteObject(lb);
        } else {
          bool hasSub = false;
          if (dis->hwndItem) {
            HMENU hm = reinterpret_cast<HMENU>(dis->hwndItem);
            int n = GetMenuItemCount(hm);
            for (int i = 0; i < n; ++i) {
              MENUITEMINFOW info{};
              info.cbSize = sizeof(info);
              info.fMask = MIIM_DATA | MIIM_SUBMENU;
              if (GetMenuItemInfoW(hm, (UINT)i, TRUE, &info) &&
                  info.dwItemData == dis->itemData && info.hSubMenu) {
                hasSub = true;
                break;
              }
            }
          }
          SetBkMode(dis->hDC, TRANSPARENT);
          SetTextColor(dis->hDC, hot ? COL_PRIMARY : COL_TEXT);
          SelectObject(dis->hDC, hFontUi ? hFontUi : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
          RECT tr = dis->rcItem;
          tr.left += 16;
          tr.right -= hasSub ? 22 : 10;
          DrawTextW(dis->hDC, text, -1, &tr,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
          if (hasSub) {
            int cx = dis->rcItem.right - 14;
            int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            HPEN pen = CreatePen(PS_SOLID, 2, hot ? COL_PRIMARY : COL_TEXT_DIM);
            HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
            MoveToEx(dis->hDC, cx - 3, cy - 4, nullptr);
            LineTo(dis->hDC, cx + 1, cy);
            LineTo(dis->hDC, cx - 3, cy + 4);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(pen);
          }
        }
        return TRUE;
      }
      return TRUE;
    }

    case WM_MOUSEMOVE: {
      POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      int hot = 0;
      if (pt.y >= 0 && pt.y < MENU_STRIP_H) {
        if (pt.x >= 8 && pt.x < 78) hot = 1;
        else if (pt.x >= 78 && pt.x < 152) hot = 2;
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
      }
      if (hot != g_menuHot) {
        g_menuHot = hot;
        RECT r{ 0, 0, 220, MENU_STRIP_H };
        InvalidateRect(hwnd, &r, FALSE);
      }
      if (hot) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      if (g_menuHot) {
        g_menuHot = 0;
        RECT r{ 0, 0, 220, MENU_STRIP_H };
        InvalidateRect(hwnd, &r, FALSE);
      }
      return 0;

    case WM_LBUTTONDOWN: {
      POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      int which = 0;
      if (pt.y >= 0 && pt.y < MENU_STRIP_H) {
        if (pt.x >= 8 && pt.x < 78) which = 1;
        else if (pt.x >= 78 && pt.x < 152) which = 2;
      }
      if (which && g_menuBar) {
        HMENU sub = GetSubMenu(g_menuBar, which - 1);
        POINT sp{ which == 1 ? 8 : 78, MENU_STRIP_H - 1 };
        ClientToScreen(hwnd, &sp);
        TrackPopupMenu(sub, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                       sp.x, sp.y, 0, hwnd, nullptr);
        return 0;
      }
      break;
    }

    case WM_COMMAND: {
      switch (LOWORD(wParam)) {
        case ID_BTN_LOGIN:
          DoLoginLoadProxy();
          break;
        case ID_CHECK_REMEMBER:
          g_rememberAccount = (SendMessageW(hCheckRemember, BM_GETCHECK, 0, 0) == BST_CHECKED);
          if (!g_rememberAccount) {
            // Immediately clear stored account secrets when unchecked
            SaveAccountCredentials();
            AppendResult(L"Account credentials will not be saved (Remember me off).");
          }
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
        case ID_BTN_COPY_REPORT:
          DoCopyFullReport();
          break;
        case ID_BTN_QUIT_APP:
          DoQuitSelectedApp();
          break;
        case ID_BTN_VERIFY_IP:
          DoVerifyExitIp();
          break;
        case ID_BTN_SAVE_PROFILE:
          DoSaveProfile();
          break;
        case ID_BTN_RUN_PROFILE:
          DoRunSelectedProfile();
          break;
        case ID_BTN_DEL_PROFILE:
          DoDeleteProfile();
          break;
        case ID_CHECK_TRAY:
          g_minimizeToTray = (SendMessageW(hCheckTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveAppRoutePrefs();
          break;
        case IDM_TRAY_SHOW:
          TrayShowWindow(hwnd);
          break;
        case IDM_TRAY_EXIT:
          DestroyWindow(hwnd);
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
          if (hCheckRemember)
            g_rememberAccount = (SendMessageW(hCheckRemember, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveIni(ReadConfigFromUI());
          SetStatus(L"Settings saved.", COL_SUCCESS);
          AppendResult(L"Settings saved to ProxyPiTester.ini");
          if (g_rememberAccount)
            AppendResult(L"  Account email/password saved (Remember me).");
          else
            AppendResult(L"  Account password not saved (Remember me off).");
          AppendResult(L"  Proxy host/user/password saved.");
          break;
        }
        case ID_BTN_LOAD: {
          ProxyConfig c;
          if (LoadIni(c)) {
            WriteConfigToUI(c);
            SetPortPairAndRefresh(g_socksPort, g_httpPort);
            SetStatus(L"Settings reloaded.", COL_SUCCESS);
            AppendResult(L"Settings reloaded from ProxyPiTester.ini");
          } else {
            LoadAccountCredentials();
            SetStatus(L"No proxy settings found (account may still load).", COL_TEXT_DIM);
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
          AppendResult(L"  File      : 100mb.bin");
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
          // Persist proxy + optional account credentials
          if (hCheckRemember)
            g_rememberAccount = (SendMessageW(hCheckRemember, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveIni(ReadConfigFromUI());
          if (g_rememberAccount)
            AppendResult(L"Credentials saved (Remember me).");
          // Product #5: auto connectivity test after successful login
          if (g_autoTestAfterLogin) {
            AppendResult(L"Auto-running connectivity test...");
            DoTest();
          }
        } else {
          SetStatus(L"Login failed: " + r->message, COL_ERROR);
          AppendResult(L"FAILED - " + r->message);
        }
        delete r;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_APP_VERIFY_DONE: {
      ProxyTestResult* r = reinterpret_cast<ProxyTestResult*>(lParam);
      if (r) {
        AppendResult(r->message);
        if (r->success) {
          g_hasResult = true;
          g_lastOk = true;
          g_lastIp = r->detectedIp;
          g_lastLatency = r->latencyMs;
          SetStatus(r->message, COL_SUCCESS);
          RefreshSessionList();
          RefreshChromePathLabel();
        } else {
          SetStatus(r->message, COL_ERROR);
        }
        delete r;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_APP_DL_UPDATE: {
      UpdateDownloadResult* r = reinterpret_cast<UpdateDownloadResult*>(lParam);
      if (r) {
        AppendResult(r->message);
        if (r->ok) {
          SetStatus(L"Update zip downloaded.", COL_SUCCESS);
          ShellExecuteW(hwnd, L"explore", r->folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
          SetStatus(L"Download failed - opening releases page.", COL_ERROR);
          OpenUrlInDefaultBrowser(hwnd, DEFAULT_DOWNLOAD_URL);
        }
        delete r;
      }
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
          msg += L"Yes = open download page\r\n";
          msg += L"No  = try download zip to Updates folder\r\n";
          msg += L"Cancel = dismiss";
          int choice = MessageBoxW(hwnd, msg.c_str(), L"Update Available",
                                   MB_YESNOCANCEL | MB_ICONINFORMATION);
          std::wstring openUrl = r->downloadUrl.empty() ? DEFAULT_DOWNLOAD_URL : r->downloadUrl;
          if (choice == IDYES) {
            AppendResult(L"Opening download page...");
            AppendResult(L"  " + openUrl);
            if (!OpenUrlInDefaultBrowser(hwnd, openUrl)) {
              SetStatus(L"Could not open browser - copy the URL from Results.", COL_ERROR);
              AppendResult(L"FAILED - could not open browser. Copy URL from Results.");
              MessageBoxW(hwnd,
                (L"Could not open the browser automatically.\r\n\r\n"
                 L"Copy this URL:\r\n\r\n" + openUrl).c_str(),
                L"Check for Updates", MB_OK | MB_ICONWARNING);
            } else {
              SetStatus(L"Opened download page in your browser.", COL_SUCCESS);
            }
          } else if (choice == IDNO) {
            DoDownloadUpdateZip(openUrl);
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
      RECT want{ 0, 0, MIN_CLIENT_W, MIN_CLIENT_H + MENU_STRIP_H };
      AdjustWindowRectEx(&want, style, FALSE, 0);
      mmi->ptMinTrackSize.x = want.right - want.left;
      mmi->ptMinTrackSize.y = want.bottom - want.top;
      // No max size lock — allow maximize and free resize
      return 0;
    }

    case WM_DESTROY: {
      KillTimer(hwnd, TIMER_SESSIONS);
      KillTimer(hwnd, TIMER_HEALTH);
      TrayRemove();
      if (hComboApps)
        g_lastAppSel = (int)SendMessageW(hComboApps, CB_GETCURSEL, 0, 0);
      SaveAppRoutePrefs();
      std::wstring clearErr;
      ClearRuneLiteSocksWrap(clearErr);
      ClearRoutedAppSessions();
      StopLocalAuthBridge();
      if (g_menuBar) {
        DestroyMenu(g_menuBar);
        g_menuBar = nullptr;
      }
      if (hFontTitle) DeleteObject(hFontTitle);
      if (hFontUi) DeleteObject(hFontUi);
      if (hFontBtn) DeleteObject(hFontBtn);
      if (hFontSmall) DeleteObject(hFontSmall);
      if (hFontSection) DeleteObject(hFontSection);
      if (hFontMetric) DeleteObject(hFontMetric);
      if (hFontMono) DeleteObject(hFontMono);
      if (hBrushBg) DeleteObject(hBrushBg);
      if (hBrushCard) DeleteObject(hBrushCard);
      if (hBrushInput) DeleteObject(hBrushInput);
      if (hBrushPrimary) DeleteObject(hBrushPrimary);
      if (g_headerIcon) {
        DestroyIcon(g_headerIcon);
        g_headerIcon = nullptr;
      }
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Startup log next to the EXE — helps diagnose silent close / AV / missing DLL
static void StartupLog(const wchar_t* line) {
  try {
    std::wstring path = GetExeDir() + L"\\ProxyPiTester-startup.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, line);
    fclose(f);
  } catch (...) {}
}

static void WriteMiniDump(EXCEPTION_POINTERS* ep) {
  std::wstring path = GetExeDir() + L"\\ProxyPiTester-crash.dmp";
  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return;
  HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
  if (dbg) {
    typedef BOOL(WINAPI* MiniDumpWriteDump_t)(
        HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
        PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (fn) {
      MINIDUMP_EXCEPTION_INFORMATION mei{};
      mei.ThreadId = GetCurrentThreadId();
      mei.ExceptionPointers = ep;
      mei.ClientPointers = FALSE;
      fn(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal,
         ep ? &mei : nullptr, nullptr, nullptr);
    }
    FreeLibrary(dbg);
  }
  CloseHandle(hFile);
  StartupLog((L"minidump: " + path).c_str());
}

static LONG WINAPI ProxyPiUnhandledException(EXCEPTION_POINTERS* ep) {
  wchar_t buf[640];
  DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
  WriteMiniDump(ep);
  swprintf_s(buf,
             L"ProxyPiTester hit an error (0x%08X) and must close.\r\n\r\n"
             L"Next to the EXE you will find:\r\n"
             L"  • ProxyPiTester-startup.log\r\n"
             L"  • ProxyPiTester-crash.dmp (if created)\r\n\r\n"
             L"Also check: right-click EXE → Properties → Unblock.\r\n"
             L"If you only minimized it: look for the tray icon near the clock.",
             code);
  StartupLog(buf);
  MessageBoxW(nullptr, buf, L"ProxyPiTester crashed", MB_OK | MB_ICONERROR);
  return EXCEPTION_EXECUTE_HANDLER;
}

// Immersive dark mode for the caption, menu bar, and scrollbars.
// Ordinals are the supported-in-practice uxtheme entry points (Win10 1903+ / Win11).
static void EnableDarkAppMode() {
  HMODULE ux = LoadLibraryW(L"uxtheme.dll");
  if (!ux) return;
  using FnSetPreferred = int (WINAPI*)(int);
  using FnRefresh = void (WINAPI*)();
  using FnFlush = void (WINAPI*)();
  auto setPreferred = (FnSetPreferred)GetProcAddress(ux, MAKEINTRESOURCEA(135));
  auto refresh = (FnRefresh)GetProcAddress(ux, MAKEINTRESOURCEA(104));
  auto flush = (FnFlush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
  if (setPreferred) setPreferred(2); // ForceDark
  if (refresh) refresh();
  if (flush) flush();
}

static void AllowDarkWindow(HWND hwnd) {
  if (!hwnd) return;
  HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
  if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
  if (!ux) return;
  using FnAllow = BOOL (WINAPI*)(HWND, BOOL);
  auto allow = (FnAllow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
  if (allow) allow(hwnd, TRUE);
}

static void ApplyDarkFrame(HWND hwnd) {
  if (!hwnd) return;
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
  DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // older Win10 build
  COLORREF caption = COL_BG;
  COLORREF border = COL_BG;
  COLORREF text = COL_TEXT;
  DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption)); // DWMWA_CAPTION_COLOR
  DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));   // DWMWA_BORDER_COLOR
  DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));       // DWMWA_TEXT_COLOR
  int corner = 2; // DWMWCP_ROUND
  DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
  AllowDarkWindow(hwnd);
  SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
  // Win11 composition flag. Darkens classic scrollbars when the OS honors it.
  HMODULE user = GetModuleHandleW(L"user32.dll");
  if (user) {
    struct WcaData { DWORD attrib; PVOID data; SIZE_T size; };
    using FnSet = BOOL (WINAPI*)(HWND, WcaData*);
    auto setWca = (FnSet)GetProcAddress(user, "SetWindowCompositionAttribute");
    BOOL darkColors = TRUE;
    WcaData data{ 26, &darkColors, sizeof(darkColors) }; // WCA_USEDARKMODECOLORS
    if (setWca) setWca(hwnd, &data);
  }
}

// Themed menu bar ignores dark mode and stays white. These undocumented
// messages are how the system asks the window to paint that strip.
#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
#endif

typedef union tagUAHMENUITEMMETRICS {
  struct { DWORD cx; DWORD cy; } rgsizeBar[2];
  struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;

typedef struct tagUAHMENUPOPUPMETRICS {
  DWORD rgcx[4];
  DWORD fUpdateMaxWidths : 2;
} UAHMENUPOPUPMETRICS;

typedef struct tagUAHMENU {
  HMENU hmenu;
  HDC hdc;
  DWORD dwFlags;
} UAHMENU;

typedef struct tagUAHMENUITEM {
  int iPosition;
  UAHMENUITEMMETRICS umim;
  UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

typedef struct tagUAHDRAWMENUITEM {
  DRAWITEMSTRUCT dis;
  UAHMENU um;
  UAHMENUITEM umi;
} UAHDRAWMENUITEM;

static LRESULT HandleMenuBarPaint(HWND hwnd, UINT msg, LPARAM lParam) {
  if (msg == 0x0091) {
    auto* udm = reinterpret_cast<UAHMENU*>(lParam);
    MENUBARINFO mbi{};
    mbi.cbSize = sizeof(mbi);
    RECT rc{};
    if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
      RECT wr{};
      GetWindowRect(hwnd, &wr);
      rc = mbi.rcBar;
      OffsetRect(&rc, -wr.left, -wr.top);
    }
    HBRUSH br = CreateSolidBrush(COL_BG);
    FillRect(udm->hdc, &rc, br);
    DeleteObject(br);
    return 0;
  }

  auto* udmi = reinterpret_cast<UAHDRAWMENUITEM*>(lParam);
  wchar_t text[128] = L"";
  MENUITEMINFOW mii{};
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_STRING;
  mii.dwTypeData = text;
  mii.cch = 127;
  GetMenuItemInfoW(udmi->um.hmenu, (UINT)udmi->umi.iPosition, TRUE, &mii);

  bool hot = (udmi->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
  HBRUSH br = CreateSolidBrush(hot ? RGB(24, 40, 62) : COL_BG);
  FillRect(udmi->um.hdc, &udmi->dis.rcItem, br);
  DeleteObject(br);

  SetBkMode(udmi->um.hdc, TRANSPARENT);
  SetTextColor(udmi->um.hdc, hot ? COL_PRIMARY : COL_TEXT);
  SelectObject(udmi->um.hdc, hFontUi ? hFontUi : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
  RECT tr = udmi->dis.rcItem;
  DrawTextW(udmi->um.hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
  SetUnhandledExceptionFilter(ProxyPiUnhandledException);
  StartupLog(L"=== start ===");
  StartupLog((std::wstring(L"version ") + APP_VERSION).c_str());
  EnableDarkAppMode();

  // DPI awareness (#24) — per-monitor v2 when available
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#elif defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
#else
  SetProcessDPIAware();
#endif

  INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
  if (!InitCommonControlsEx(&icc)) {
    StartupLog(L"InitCommonControlsEx failed (continuing)");
  }

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

  if (!RegisterClassExW(&wc)) {
    DWORD err = GetLastError();
    // Class already registered (second instance) — try to find existing window
    if (err == ERROR_CLASS_ALREADY_EXISTS) {
      HWND existing = FindWindowW(L"ProxyPiTesterMain", nullptr);
      if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        StartupLog(L"second instance - activated existing window");
        return 0;
      }
    }
    wchar_t msg[256];
    swprintf_s(msg, L"RegisterClassEx failed (error %lu). App cannot start.", err);
    StartupLog(msg);
    MessageBoxW(nullptr, msg, L"ProxyPiTester", MB_OK | MB_ICONERROR);
    return 1;
  }

  // Resizable + maximizable; larger default client area
  const DWORD style = WS_OVERLAPPEDWINDOW;
  RECT wr{ 0, 0, DEFAULT_CLIENT_W, DEFAULT_CLIENT_H + MENU_STRIP_H };
  AdjustWindowRectEx(&wr, style, FALSE, 0);
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

  StartupLog(L"CreateWindowEx...");
  HWND hwnd = CreateWindowExW(
    WS_EX_CONTROLPARENT, L"ProxyPiTesterMain",
    L"ProxyPi Tester",
    style,
    x, y, winW, winH,
    nullptr, nullptr, hInstance, nullptr);

  if (!hwnd) {
    DWORD err = GetLastError();
    wchar_t msg[320];
    swprintf_s(msg,
      L"Could not create the main window (error %lu).\r\n\r\n"
      L"Common fixes:\r\n"
      L"• Right-click ProxyPiTester.exe → Properties → Unblock → OK\r\n"
      L"• Extract the zip to a folder first (do not run from inside the zip)\r\n"
      L"• Windows Security may be blocking unsigned apps — allow this file\r\n"
      L"• See ProxyPiTester-startup.log next to the EXE",
      err);
    StartupLog(msg);
    MessageBoxW(nullptr, msg, L"ProxyPiTester failed to start", MB_OK | MB_ICONERROR);
    return 1;
  }
  StartupLog(L"window created OK");
  ApplyDarkFrame(hwnd);
  g_menuBar = CreateAppMenu();

  SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);

  ShowWindow(hwnd, nCmdShow == SW_HIDE ? SW_SHOW : nCmdShow);
  UpdateWindow(hwnd);
  // Quiet update check after UI is visible
  DoCheckUpdates(true);

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0) > 0) {
    // Enables Tab / Shift+Tab between email, password, and other controls
    if (hMain && IsDialogMessageW(hMain, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  StartupLog(L"=== exit ===");
  return (int)msg.wParam;
}

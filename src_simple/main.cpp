// ProxyPi Tester — modern, branding-aligned proxy connectivity + speed test tool
// Companion to the full ProxyTools router app (left intact).
// Branding colors from proxypi.co.uk

#include "common.h"
#include "proxy_client.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- App version & update feed (public GitHub — no credentials required) ----
// Bump APP_VERSION when you ship a new build. Host update.json on a public repo/raw URL.
static const wchar_t* APP_VERSION = L"1.0.0";
static const wchar_t* APP_NAME = L"ProxyPiTester";
// Primary: simple JSON on raw.githubusercontent.com
// Fallback: GitHub Releases API for the same repo
static const wchar_t* UPDATE_JSON_HOST = L"raw.githubusercontent.com";
static const wchar_t* UPDATE_JSON_PATH = L"/conthegreat/proxypitester/main/update.json";
static const wchar_t* GITHUB_API_HOST = L"api.github.com";
static const wchar_t* GITHUB_API_PATH = L"/repos/conthegreat/proxypitester/releases/latest";
static const wchar_t* DEFAULT_DOWNLOAD_URL = L"https://github.com/conthegreat/proxypitester/releases/latest";
static const wchar_t* WEBSITE_URL = L"https://proxypi.co.uk";

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

// Menu IDs
#define IDM_FILE_EXIT       3001
#define IDM_HELP_UPDATE     3010
#define IDM_HELP_WEBSITE    3011
#define IDM_HELP_ABOUT      3012

// Custom messages from worker threads
#define WM_APP_TEST_DONE    (WM_APP + 10)
#define WM_APP_SPEED_DONE   (WM_APP + 11)
#define WM_APP_UPDATE_DONE  (WM_APP + 12)

static HWND hMain = nullptr;
static HWND hComboType = nullptr;
static HWND hHost = nullptr, hPort = nullptr;
static HWND hCheckAuth = nullptr;
static HWND hUser = nullptr, hPass = nullptr;
static HWND hBtnTest = nullptr, hBtnSpeed = nullptr;
static HWND hBtnSave = nullptr, hBtnLoad = nullptr, hBtnSite = nullptr;
static HWND hResult = nullptr;
static HWND hStatus = nullptr;

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

// Layout constants (CLIENT area size — window chrome is added via AdjustWindowRectEx)
static const int CLIENT_W = 560;
static const int CLIENT_H = 760;
static const int PAD = 24;
static const int CARD_X = 20;
static const int CARD_W = CLIENT_W - 40;

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
  return !cfg.host.empty();
}

static void SetStatus(const std::wstring& text, COLORREF color = COL_TEXT_DIM) {
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
  msg += L"ProxyPiTester  v";
  msg += APP_VERSION;
  msg += L"\r\n\r\n";
  msg += L"UK residential proxy connectivity and speed test tool.\r\n";
  msg += L"Built for ProxyPi — https://proxypi.co.uk\r\n\r\n";
  msg += L"Features:\r\n";
  msg += L"  - SOCKS5 / HTTP proxy test (exit IP + latency)\r\n";
  msg += L"  - Download speed test through your proxy\r\n";
  msg += L"  - Optional update checks via public GitHub feed\r\n\r\n";
  msg += L"No credentials are embedded in this app.\r\n";
  msg += L"Your host/user/password stay on your PC only.\r\n\r\n";
  msg += L"(c) ProxyPi  |  Help > Check for Updates for new builds";
  MessageBoxW(parent, msg.c_str(), L"About ProxyPiTester", MB_OK | MB_ICONINFORMATION);
}

static HMENU CreateAppMenu() {
  HMENU hMenubar = CreateMenu();
  HMENU hFile = CreatePopupMenu();
  HMENU hHelp = CreatePopupMenu();

  AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit");
  AppendMenuW(hHelp, MF_STRING, IDM_HELP_UPDATE, L"Check for &Updates...");
  AppendMenuW(hHelp, MF_STRING, IDM_HELP_WEBSITE, L"Visit &Website");
  AppendMenuW(hHelp, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"&About ProxyPiTester");

  AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hFile, L"&File");
  AppendMenuW(hMenubar, MF_POPUP, (UINT_PTR)hHelp, L"&Help");
  return hMenubar;
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
    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
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
  DrawTextAt(hdc, L"UK Residential Proxy Tester", PAD, 58, 400, COL_TEXT_DIM, hFontSmall);
  DrawTextAt(hdc, L"proxypi.co.uk", width - PAD - 140, 28, 140, COL_TEXT_DIM, hFontSmall, DT_RIGHT);
}

static void PaintMetricCards(HDC hdc, int y) {
  int cardH = 78;
  int gap = 10;
  int w3 = (CARD_W - gap * 2) / 3;

  auto paintOne = [&](int x, const wchar_t* label, const std::wstring& value, COLORREF valueColor) {
    RECT rc{ x, y, x + w3, y + cardH };
    FillRoundRect(hdc, rc, 12, COL_CARD, COL_BORDER);
    DrawTextAt(hdc, label, x + 14, y + 12, w3 - 28, COL_TEXT_DIM, hFontSmall);
    DrawTextAt(hdc, value.c_str(), x + 14, y + 36, w3 - 28, valueColor, hFontUi);
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
  paintOne(CARD_X, L"LATENCY", lat, okCol);
  paintOne(CARD_X + w3 + gap, L"EXIT IP", ip, g_hasResult && g_lastOk ? COL_PRIMARY : COL_TEXT_DIM);
  paintOne(CARD_X + (w3 + gap) * 2, L"DOWNLOAD", spd, g_hasResult && g_lastMbps >= 0 ? COL_PRIMARY : COL_TEXT_DIM);
}

static void PaintCardChrome(HDC hdc, RECT rc, const wchar_t* title) {
  FillRoundRect(hdc, rc, 14, COL_CARD, COL_BORDER);
  DrawTextAt(hdc, title, rc.left + 16, rc.top + 12, rc.right - rc.left - 32, COL_PRIMARY, hFontSmall);
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

      // Layout Y positions (controls sit on painted cards)
      // Config card: y=120, height ~210
      // Metric cards: y=350
      // Buttons: y=440
      // Result card: y=500

      int labelX = CARD_X + 18;
      int fieldX = CARD_X + 110;
      int fieldW = CARD_W - 130;
      int y = 158;
      int rowH = 32;
      int gapY = 36;

      auto mkLabel = [&](const wchar_t* t, int ly) {
        HWND s = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE,
          labelX, ly + 4, 90, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(s, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        return s;
      };

      mkLabel(L"Type", y);
      hComboType = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        fieldX, y, 160, 200, hwnd, (HMENU)ID_COMBO_TYPE, nullptr, nullptr);
      SendMessageW(hComboType, CB_ADDSTRING, 0, (LPARAM)L"SOCKS5");
      SendMessageW(hComboType, CB_ADDSTRING, 0, (LPARAM)L"HTTP / HTTPS");
      SendMessageW(hComboType, CB_SETCURSEL, 0, 0);
      SendMessageW(hComboType, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      y += gapY;
      mkLabel(L"Host", y);
      hHost = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        fieldX, y, fieldW, rowH, hwnd, (HMENU)ID_EDIT_HOST, nullptr, nullptr);
      SendMessageW(hHost, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      y += gapY;
      mkLabel(L"Port", y);
      hPort = CreateWindowW(L"EDIT", L"18721",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
        fieldX, y, 100, rowH, hwnd, (HMENU)ID_EDIT_PORT, nullptr, nullptr);
      SendMessageW(hPort, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      hCheckAuth = CreateWindowW(L"BUTTON", L"  Authentication",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        fieldX + 120, y + 4, 160, 24, hwnd, (HMENU)ID_CHECK_AUTH, nullptr, nullptr);
      SendMessageW(hCheckAuth, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
      SendMessageW(hCheckAuth, BM_SETCHECK, BST_CHECKED, 0);

      y += gapY;
      mkLabel(L"Username", y);
      hUser = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        fieldX, y, fieldW, rowH, hwnd, (HMENU)ID_EDIT_USER, nullptr, nullptr);
      SendMessageW(hUser, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      y += gapY;
      mkLabel(L"Password", y);
      hPass = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        fieldX, y, fieldW, rowH, hwnd, (HMENU)ID_EDIT_PASS, nullptr, nullptr);
      SendMessageW(hPass, WM_SETFONT, (WPARAM)hFontUi, TRUE);

      // Primary actions
      int btnY = 440;
      int btnW = (CARD_W - 10) / 2;
      hBtnTest = CreateStyledButton(hwnd, L"Test Proxy", CARD_X, btnY, btnW, 42, ID_BTN_TEST, true);
      hBtnSpeed = CreateStyledButton(hwnd, L"Speed Test", CARD_X + btnW + 10, btnY, btnW, 42, ID_BTN_SPEED, true);

      // Secondary row
      int secY = btnY + 52;
      int secW = (CARD_W - 20) / 3;
      hBtnSave = CreateStyledButton(hwnd, L"Save", CARD_X, secY, secW, 34, ID_BTN_SAVE, false);
      hBtnLoad = CreateStyledButton(hwnd, L"Reload", CARD_X + secW + 10, secY, secW, 34, ID_BTN_LOAD, false);
      hBtnSite = CreateStyledButton(hwnd, L"Website", CARD_X + (secW + 10) * 2, secY, secW, 34, ID_BTN_SITE, false);

      // Result log (inside RESULTS card; leave room for status strip + footer below)
      hResult = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        CARD_X + 14, 560, CARD_W - 28, 110, hwnd, (HMENU)ID_EDIT_RESULT, nullptr, nullptr);
      SendMessageW(hResult, WM_SETFONT, (WPARAM)hFontMono, TRUE);

      // Status strip — inside client area (not below the window edge)
      hStatus = CreateWindowW(L"STATIC", g_statusText.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
        CARD_X + 8, CLIENT_H - 72, CARD_W - 16, 36, hwnd, (HMENU)ID_STATIC_STATUS, nullptr, nullptr);
      SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

      // Defaults / load
      ProxyConfig cfg;
      if (LoadIni(cfg)) {
        WriteConfigToUI(cfg);
        SetStatus(L"Loaded saved settings.", COL_TEXT_DIM);
      } else {
        // Sensible ProxyPi defaults
        cfg.type = ProxyType::SOCKS5;
        cfg.host = L"";
        cfg.port = 18721;
        cfg.useAuth = true;
        WriteConfigToUI(cfg);
        SetStatus(L"Ready - enter your ProxyPi host, port and credentials.", COL_TEXT_DIM);
      }
      AppendResult(L"ProxyPiTester ready.");
      AppendResult(L"SOCKS5 + auth is recommended for residential nodes.");
      // Quiet background update check (only prompts if a newer version is published)
      DoCheckUpdates(true);
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

      PaintHeader(mem, client.right);

      // Config card
      RECT cfgCard{ CARD_X, 120, CARD_X + CARD_W, 340 };
      PaintCardChrome(mem, cfgCard, L"PROXY CONFIGURATION");

      // Metric cards
      PaintMetricCards(mem, 350);

      // Results card chrome
      RECT resCard{ CARD_X, 530, CARD_X + CARD_W, CLIENT_H - 88 };
      PaintCardChrome(mem, resCard, L"RESULTS");

      // Status strip background
      RECT statusBg{ CARD_X, CLIENT_H - 78, CARD_X + CARD_W, CLIENT_H - 38 };
      FillRoundRect(mem, statusBg, 10, COL_CARD, COL_BORDER);

      // Footer branding
      {
        std::wstring foot = L"No logging | UK residential | Pay as you go  |  v";
        foot += APP_VERSION;
        DrawTextAt(mem, foot.c_str(),
                   CARD_X, CLIENT_H - 28, CARD_W, COL_TEXT_DIM, hFontSmall, DT_CENTER);
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
        } else {
          SetStatus(L"Speed test failed: " + r->message, COL_ERROR);
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
            ShellExecuteW(hwnd, L"open", r->downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      RECT want{ 0, 0, CLIENT_W, CLIENT_H };
      AdjustWindowRectEx(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, TRUE, 0);
      int ww = want.right - want.left;
      int wh = want.bottom - want.top;
      mmi->ptMinTrackSize.x = ww;
      mmi->ptMinTrackSize.y = wh;
      mmi->ptMaxTrackSize.x = ww;
      mmi->ptMaxTrackSize.y = wh;
      return 0;
    }

    case WM_DESTROY:
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

  // Size for exact client area (+ menu bar height)
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT wr{ 0, 0, CLIENT_W, CLIENT_H };
  AdjustWindowRectEx(&wr, style, TRUE, 0); // TRUE = account for menu bar
  int winW = wr.right - wr.left;
  int winH = wr.bottom - wr.top;

  int sx = GetSystemMetrics(SM_CXSCREEN);
  int sy = GetSystemMetrics(SM_CYSCREEN);
  int x = (sx - winW) / 2;
  int y = (sy - winH) / 2;

  HWND hwnd = CreateWindowExW(
    0, L"ProxyPiTesterMain",
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
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return (int)msg.wParam;
}

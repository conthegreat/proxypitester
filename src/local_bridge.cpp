#include "local_bridge.h"
#include "proxy_client.h"
#include "common.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <map>
#include <set>
#include <iphlpapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

std::mutex g_mu;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
SOCKET g_listenSock = INVALID_SOCKET;
int g_localPort = 0;
ProxyConfig g_cfg;
ProxyConfig g_lastBridgeCfg; // copy for auto-restart
bool g_haveLastBridgeCfg = false;
std::thread g_acceptThread;
std::wstring g_lastErr;
std::atomic<bool> g_runeLiteWrapDesired{false};

void CloseSock(SOCKET& s) {
  if (s != INVALID_SOCKET) {
    closesocket(s);
    s = INVALID_SOCKET;
  }
}

bool RecvExact(SOCKET s, char* buf, int n) {
  int got = 0;
  while (got < n) {
    int r = recv(s, buf + got, n - got, 0);
    if (r <= 0) return false;
    got += r;
  }
  return true;
}

bool SendAll(SOCKET s, const char* buf, int n) {
  int sent = 0;
  while (sent < n) {
    int r = send(s, buf + sent, n - sent, 0);
    if (r <= 0) return false;
    sent += r;
  }
  return true;
}

void RelayBoth(SOCKET a, SOCKET b) {
  // Simple select-based bidirectional relay until either side closes
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(a, &rfds);
    FD_SET(b, &rfds);
    SOCKET maxfd = (a > b) ? a : b;
    timeval tv{ 30, 0 };
    int sel = select((int)maxfd + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) break;

    char buf[16384];
    if (FD_ISSET(a, &rfds)) {
      int n = recv(a, buf, sizeof(buf), 0);
      if (n <= 0) break;
      if (!SendAll(b, buf, n)) break;
    }
    if (FD_ISSET(b, &rfds)) {
      int n = recv(b, buf, sizeof(buf), 0);
      if (n <= 0) break;
      if (!SendAll(a, buf, n)) break;
    }
  }
  // Half-close
  shutdown(a, SD_BOTH);
  shutdown(b, SD_BOTH);
}

// Handle one Chrome connection: SOCKS5 no-auth server, then tunnel via upstream.
void HandleClientSocksLocal(SOCKET client, ProxyConfig cfg) {
  // Set timeouts
  int to = 30000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));

  // Greeting: VER NMETHODS METHODS...
  unsigned char hdr[2];
  if (!RecvExact(client, (char*)hdr, 2) || hdr[0] != 0x05) {
    CloseSock(client);
    return;
  }
  int nmethods = hdr[1];
  if (nmethods <= 0 || nmethods > 32) {
    CloseSock(client);
    return;
  }
  std::vector<unsigned char> methods(nmethods);
  if (!RecvExact(client, (char*)methods.data(), nmethods)) {
    CloseSock(client);
    return;
  }

  // Offer no-auth only (Chrome is happy; we auth upstream ourselves)
  unsigned char greply[2] = { 0x05, 0x00 };
  if (!SendAll(client, (char*)greply, 2)) {
    CloseSock(client);
    return;
  }

  // Request: VER CMD RSV ATYP ...
  unsigned char req[4];
  if (!RecvExact(client, (char*)req, 4) || req[0] != 0x05) {
    CloseSock(client);
    return;
  }
  unsigned char cmd = req[1];
  unsigned char atyp = req[3];

  std::string targetHost;
  int targetPort = 0;

  if (atyp == 0x01) { // IPv4
    unsigned char addr[4];
    if (!RecvExact(client, (char*)addr, 4)) { CloseSock(client); return; }
    char ip[32];
    sprintf_s(ip, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
    targetHost = ip;
  } else if (atyp == 0x03) { // Domain
    unsigned char len = 0;
    if (!RecvExact(client, (char*)&len, 1) || len == 0) { CloseSock(client); return; }
    std::vector<char> dom(len);
    if (!RecvExact(client, dom.data(), len)) { CloseSock(client); return; }
    targetHost.assign(dom.data(), len);
  } else if (atyp == 0x04) { // IPv6 — skip for now (read & reject)
    unsigned char addr[16];
    RecvExact(client, (char*)addr, 16);
    unsigned char portb[2];
    RecvExact(client, (char*)portb, 2);
    unsigned char fail[10] = { 0x05, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0 }; // addr type not supported
    SendAll(client, (char*)fail, 10);
    CloseSock(client);
    return;
  } else {
    CloseSock(client);
    return;
  }

  unsigned char portb[2];
  if (!RecvExact(client, (char*)portb, 2)) { CloseSock(client); return; }
  targetPort = (portb[0] << 8) | portb[1];

  if (cmd != 0x01) { // only CONNECT
    unsigned char fail[10] = { 0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    SendAll(client, (char*)fail, 10);
    CloseSock(client);
    return;
  }

  // Connect to real proxy and tunnel
  std::wstring err;
  SOCKET upstream = INVALID_SOCKET;
  if (!ConnectTcp(cfg.hostA(), cfg.port, upstream, err)) {
    Log(L"[bridge] upstream TCP failed: " + err);
    unsigned char fail[10] = { 0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0 }; // connection refused
    SendAll(client, (char*)fail, 10);
    CloseSock(client);
    return;
  }

  setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
  setsockopt(upstream, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));

  bool tunnelOk = false;
  if (cfg.type == ProxyType::SOCKS5) {
    tunnelOk = Socks5Connect(upstream, cfg, targetHost, targetPort, err);
  } else {
    tunnelOk = HttpProxyConnect(upstream, cfg, targetHost, targetPort, err);
  }

  if (!tunnelOk) {
    Log(L"[bridge] upstream tunnel failed for " +
        std::wstring(targetHost.begin(), targetHost.end()) + L":" + std::to_wstring(targetPort) +
        L" - " + err);
    unsigned char fail[10] = { 0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    SendAll(client, (char*)fail, 10);
    CloseSock(upstream);
    CloseSock(client);
    return;
  }

  // Success reply to Chrome (bind addr 0.0.0.0:0)
  unsigned char ok[10] = { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
  if (!SendAll(client, (char*)ok, 10)) {
    CloseSock(upstream);
    CloseSock(client);
    return;
  }

  // Avoid per-connection UI logging (Chrome opens dozens of sockets; flooding Log hung the UI).
  OutputDebugStringA(("[bridge] tunnel OK " + targetHost + ":" + std::to_string(targetPort) + "\r\n").c_str());

  RelayBoth(client, upstream);
  CloseSock(upstream);
  CloseSock(client);
}

// Local HTTP CONNECT proxy (no auth) for when user selected HTTP type in UI
// but we still want a local front. Chrome: --proxy-server=http://127.0.0.1:port
void HandleClientHttpLocal(SOCKET client, ProxyConfig cfg) {
  int to = 30000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));

  // Read request line + headers until \r\n\r\n
  std::string req;
  char ch;
  while (req.size() < 8192) {
    int n = recv(client, &ch, 1, 0);
    if (n <= 0) { CloseSock(client); return; }
    req.push_back(ch);
    if (req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0) break;
  }

  // CONNECT host:port HTTP/1.x
  if (req.rfind("CONNECT ", 0) != 0) {
    const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n";
    send(client, resp, (int)strlen(resp), 0);
    CloseSock(client);
    return;
  }
  size_t sp1 = req.find(' ');
  size_t sp2 = req.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    CloseSock(client);
    return;
  }
  std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1); // host:port
  std::string host;
  int port = 443;
  size_t colon = target.rfind(':');
  if (colon != std::string::npos) {
    host = target.substr(0, colon);
    port = atoi(target.c_str() + colon + 1);
  } else {
    host = target;
  }

  std::wstring err;
  SOCKET upstream = INVALID_SOCKET;
  if (!ConnectTcp(cfg.hostA(), cfg.port, upstream, err)) {
    const char* resp = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";
    send(client, resp, (int)strlen(resp), 0);
    CloseSock(client);
    return;
  }
  setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));

  bool tunnelOk = false;
  if (cfg.type == ProxyType::SOCKS5) {
    tunnelOk = Socks5Connect(upstream, cfg, host, port, err);
  } else {
    tunnelOk = HttpProxyConnect(upstream, cfg, host, port, err);
  }
  if (!tunnelOk) {
    const char* resp = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";
    send(client, resp, (int)strlen(resp), 0);
    CloseSock(upstream);
    CloseSock(client);
    return;
  }

  const char* ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
  if (send(client, ok, (int)strlen(ok), 0) <= 0) {
    CloseSock(upstream);
    CloseSock(client);
    return;
  }

  OutputDebugStringA(("[bridge/http] tunnel OK " + host + ":" + std::to_string(port) + "\r\n").c_str());
  RelayBoth(client, upstream);
  CloseSock(upstream);
  CloseSock(client);
}

void AcceptLoop() {
  // Quiet on success - failures still Log() so Results stays readable
  OutputDebugStringW((L"[bridge] acceptor started on 127.0.0.1:" +
                      std::to_wstring(g_localPort) + L"\r\n").c_str());

  while (!g_stop.load()) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_listenSock, &rfds);
    timeval tv{ 1, 0 };
    int sel = select((int)g_listenSock + 1, &rfds, nullptr, nullptr, &tv);
    if (sel < 0) break;
    if (sel == 0) continue;

    sockaddr_in peer{};
    int plen = sizeof(peer);
    SOCKET client = accept(g_listenSock, (sockaddr*)&peer, &plen);
    if (client == INVALID_SOCKET) continue;

    ProxyConfig cfgCopy;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      cfgCopy = g_cfg;
    }

    // One thread per Chrome connection (Chromium opens many)
    std::thread([client, cfgCopy]() {
      // Local side always speaks SOCKS5 no-auth to Chrome when we launch with socks5://127.0.0.1
      HandleClientSocksLocal(client, cfgCopy);
    }).detach();
  }

  OutputDebugStringW(L"[bridge] acceptor stopped\r\n");
  g_running = false;
}

} // namespace

bool EnsureLocalAuthBridge(const ProxyConfig& cfg, int& outLocalPort, std::wstring& err) {
  outLocalPort = 0;
  err.clear();

  std::wstring wsaErr;
  if (!InitWinsock(wsaErr)) {
    err = wsaErr;
    return false;
  }

  int portForLog = 0;
  bool reused = false;
  std::wstring hostForLog;
  int upPortForLog = 0;
  bool authForLog = false;

  {
    std::lock_guard<std::mutex> lock(g_mu);

    // Already running: just refresh upstream config (per-connection copy) and reuse port
    if (g_running && g_listenSock != INVALID_SOCKET && g_localPort > 0) {
      g_cfg = cfg;
      g_lastBridgeCfg = cfg;
      g_haveLastBridgeCfg = true;
      outLocalPort = g_localPort;
      portForLog = g_localPort;
      reused = true;
      hostForLog = cfg.host;
      upPortForLog = cfg.port;
      authForLog = cfg.useAuth;
    } else {
      g_stop = false;
      g_cfg = cfg;
      g_lastBridgeCfg = cfg;
      g_haveLastBridgeCfg = true;

      SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (ls == INVALID_SOCKET) {
        err = L"socket() failed";
        return false;
      }

      BOOL yes = 1;
      setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
      addr.sin_port = 0; // ephemeral

      if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0) {
        err = L"bind() failed: " + std::to_wstring(WSAGetLastError());
        closesocket(ls);
        return false;
      }

      if (listen(ls, 128) != 0) {
        err = L"listen() failed";
        closesocket(ls);
        return false;
      }

      sockaddr_in bound{};
      int blen = sizeof(bound);
      getsockname(ls, (sockaddr*)&bound, &blen);
      g_localPort = ntohs(bound.sin_port);
      g_listenSock = ls;
      g_running = true;

      // Start acceptor WITHOUT holding work that Logs under this lock
      g_acceptThread = std::thread(AcceptLoop);
      g_acceptThread.detach();

      outLocalPort = g_localPort;
      portForLog = g_localPort;
      hostForLog = cfg.host;
      upPortForLog = cfg.port;
      authForLog = cfg.useAuth;
    }
  } // release g_mu BEFORE Log (avoids deadlock with worker Log/PostMessage)

  // Debug-only: Results window gets a single clean "Chrome started" line from the UI
  std::wstring dbg = reused
    ? (L"[bridge] reusing 127.0.0.1:" + std::to_wstring(portForLog) +
       L" -> " + hostForLog + L":" + std::to_wstring(upPortForLog) + L"\r\n")
    : (L"[bridge] listening on 127.0.0.1:" + std::to_wstring(portForLog) +
       L" -> upstream " + hostForLog + L":" + std::to_wstring(upPortForLog) +
       (authForLog ? L" (with auth)" : L"") + L"\r\n");
  OutputDebugStringW(dbg.c_str());
  return true;
}

void StopLocalAuthBridge() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_stop = true;
  CloseSock(g_listenSock);
  g_localPort = 0;
  g_running = false;
}

bool IsLocalAuthBridgeRunning() {
  return g_running.load() && g_localPort > 0;
}

int GetLocalAuthBridgePort() {
  return (g_running.load() && g_localPort > 0) ? g_localPort : 0;
}

bool GetLastBridgeConfig(ProxyConfig& out) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_haveLastBridgeCfg || g_lastBridgeCfg.host.empty()) return false;
  out = g_lastBridgeCfg;
  return true;
}

bool EnsureBridgeAlive(std::wstring& err) {
  err.clear();
  if (IsLocalAuthBridgeRunning()) return true;
  ProxyConfig cfg;
  if (!GetLastBridgeConfig(cfg)) {
    err = L"No previous bridge config to restart";
    return false;
  }
  int port = 0;
  return EnsureLocalAuthBridge(cfg, port, err) && port > 0;
}

int TerminateProcessesByImage(const std::wstring& imageBase, DWORD waitMs) {
  if (imageBase.empty()) return 0;
  std::vector<HANDLE> handles;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  int count = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, imageBase.c_str()) != 0) continue;
      HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
      if (!h) continue;
      if (TerminateProcess(h, 1)) {
        ++count;
        handles.push_back(h);
      } else {
        CloseHandle(h);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (!handles.empty() && waitMs > 0) {
    WaitForMultipleObjects((DWORD)handles.size(), handles.data(), TRUE, waitMs);
  }
  for (HANDLE h : handles) CloseHandle(h);
  return count;
}

// ---- Multi-app session registry ----
namespace {
std::mutex g_sessMu;
std::vector<RoutedAppSession> g_sessions;

bool PidAlive(DWORD pid) {
  if (pid == 0) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) {
    // Fall back: process may still exist; toolhelp check
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
      do {
        if (pe.th32ProcessID == pid) { found = true; break; }
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
  }
  DWORD code = 0;
  BOOL ok = GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return ok && code == STILL_ACTIVE;
}

std::wstring ProcessImageBaseName(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};
  wchar_t path[MAX_PATH] = {};
  DWORD sz = MAX_PATH;
  std::wstring base;
  if (QueryFullProcessImageNameW(h, 0, path, &sz)) {
    base = path;
    size_t slash = base.find_last_of(L"\\/");
    if (slash != std::wstring::npos) base = base.substr(slash + 1);
  }
  CloseHandle(h);
  return base;
}

std::wstring FriendlyNameFromImage(const std::wstring& image) {
  if (image.empty()) return L"Unknown";
  if (_wcsicmp(image.c_str(), L"chrome.exe") == 0) return L"Chrome";
  if (_wcsicmp(image.c_str(), L"msedge.exe") == 0) return L"Edge";
  if (_wcsicmp(image.c_str(), L"brave.exe") == 0) return L"Brave";
  if (_wcsicmp(image.c_str(), L"firefox.exe") == 0) return L"Firefox";
  if (_wcsicmp(image.c_str(), L"Discord.exe") == 0) return L"Discord";
  if (_wcsicmp(image.c_str(), L"opera.exe") == 0) return L"Opera";
  if (_wcsicmp(image.c_str(), L"vivaldi.exe") == 0) return L"Vivaldi";
  if (_wcsicmp(image.c_str(), L"slack.exe") == 0) return L"Slack";
  if (_wcsicmp(image.c_str(), L"Teams.exe") == 0) return L"Teams";
  if (_wcsicmp(image.c_str(), L"ms-teams.exe") == 0) return L"Teams";
  if (_wcsicmp(image.c_str(), L"Code.exe") == 0) return L"VS Code";
  if (_wcsicmp(image.c_str(), L"Cursor.exe") == 0) return L"Cursor";
  if (_wcsicmp(image.c_str(), L"Postman.exe") == 0) return L"Postman";
  if (_wcsicmp(image.c_str(), L"thunderbird.exe") == 0) return L"Thunderbird";
  if (_wcsicmp(image.c_str(), L"Spotify.exe") == 0) return L"Spotify";
  if (_wcsicmp(image.c_str(), L"RuneLite.exe") == 0) return L"RuneLite";
  if (_wcsicmp(image.c_str(), L"java.exe") == 0) return L"RuneLite";
  if (_wcsicmp(image.c_str(), L"JagexLauncher.exe") == 0) return L"Jagex";
  // strip .exe
  std::wstring n = image;
  if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".exe") == 0)
    n = n.substr(0, n.size() - 4);
  return n;
}

// PIDs with established TCP to 127.0.0.1:bridgePort (clients of our SOCKS bridge)
struct BridgeClientInfo {
  DWORD pid = 0;
  int conns = 0;
  std::wstring image;
};

std::vector<BridgeClientInfo> EnumBridgeClients(int bridgePort) {
  std::vector<BridgeClientInfo> out;
  if (bridgePort <= 0) return out;

  DWORD size = 0;
  // Probe size
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
  if (size == 0) return out;
  std::vector<char> buf(size);
  auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
  if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR)
    return out;

  std::map<DWORD, int> pidConns;
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto& row = table->table[i];
    // Client side: remote is our bridge (or local is our listen + peer)
    u_short localPort = ntohs((u_short)row.dwLocalPort);
    u_short remotePort = ntohs((u_short)row.dwRemotePort);
    // Only care about established (or close-wait still holding) sockets
    if (row.dwState != MIB_TCP_STATE_ESTAB && row.dwState != MIB_TCP_STATE_CLOSE_WAIT)
      continue;

    bool toBridge = false;
    // Connection TO 127.0.0.1:bridgePort
    if (remotePort == (u_short)bridgePort && row.dwRemoteAddr == htonl(INADDR_LOOPBACK))
      toBridge = true;
    // Or from our side (bridge process) — skip; we want client PIDs only
    if (localPort == (u_short)bridgePort && row.dwLocalAddr == htonl(INADDR_LOOPBACK))
      continue;

    if (!toBridge) continue;
    if (row.dwOwningPid == 0) continue;
    pidConns[row.dwOwningPid] += 1;
  }

  for (auto& kv : pidConns) {
    BridgeClientInfo c;
    c.pid = kv.first;
    c.conns = kv.second;
    c.image = ProcessImageBaseName(kv.first);
    // Skip our own process if it ever appears
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring selfBase = self;
    size_t slash = selfBase.find_last_of(L"\\/");
    if (slash != std::wstring::npos) selfBase = selfBase.substr(slash + 1);
    if (!c.image.empty() && _wcsicmp(c.image.c_str(), selfBase.c_str()) == 0)
      continue;
    out.push_back(c);
  }
  return out;
}

// Also find processes by image name (for just-launched apps before first SOCKS connect)
std::vector<DWORD> FindPidsByImage(const std::wstring& imageBase) {
  std::vector<DWORD> pids;
  if (imageBase.empty()) return pids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return pids;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, imageBase.c_str()) == 0)
        pids.push_back(pe.th32ProcessID);
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return pids;
}

std::wstring ImageHintForAppName(const std::wstring& name) {
  if (name == L"Chrome") return L"chrome.exe";
  if (name == L"Edge") return L"msedge.exe";
  if (name == L"Brave") return L"brave.exe";
  if (name == L"Opera") return L"opera.exe";
  if (name == L"Vivaldi") return L"vivaldi.exe";
  if (name == L"Firefox") return L"firefox.exe";
  if (name == L"Discord") return L"Discord.exe";
  if (name == L"Slack") return L"slack.exe";
  if (name == L"Teams") return L"Teams.exe";
  if (name == L"VS Code") return L"Code.exe";
  if (name == L"Cursor") return L"Cursor.exe";
  if (name == L"Postman") return L"Postman.exe";
  if (name == L"Thunderbird") return L"thunderbird.exe";
  if (name == L"Spotify") return L"Spotify.exe";
  if (name == L"RuneLite") return L"RuneLite.exe";
  if (name == L"Jagex") return L"JagexLauncher.exe";
  return {};
}

} // namespace

// Newest app-*\Exe under a root (Discord / Slack / Postman layout).
static std::wstring FindNewestAppDirExe(const std::wstring& root, const std::wstring& exeName) {
  if (root.empty() || exeName.empty()) return {};
  std::wstring best;
  FILETIME bestWrite{};
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((root + L"\\app-*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
      std::wstring cand = root + L"\\" + fd.cFileName + L"\\" + exeName;
      if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestWrite) > 0) {
        best = cand;
        bestWrite = fd.ftLastWriteTime;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (!best.empty()) return best;
  std::wstring fallback = root + L"\\" + exeName;
  if (GetFileAttributesW(fallback.c_str()) != INVALID_FILE_ATTRIBUTES) return fallback;
  return {};
}

bool IsImageRunning(const std::wstring& imageBase) {
  if (imageBase.empty()) return false;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, imageBase.c_str()) == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

void RegisterRoutedApp(DWORD pid, const std::wstring& name, const std::wstring& method, int bridgePort) {
  if (pid == 0 && name.empty()) return;
  std::lock_guard<std::mutex> lock(g_sessMu);
  // Upsert by app name so re-launch replaces stale row instead of stacking ghosts
  for (auto& s : g_sessions) {
    if (_wcsicmp(s.name.c_str(), name.c_str()) == 0) {
      s.pid = pid;
      s.method = method;
      s.bridgePort = bridgePort;
      s.startTick = GetTickCount();
      s.alive = true;
      s.connCount = 0;
      s.processCount = 1;
      return;
    }
  }
  RoutedAppSession s;
  s.pid = pid;
  s.name = name;
  s.method = method;
  s.bridgePort = bridgePort;
  s.startTick = GetTickCount();
  s.alive = true;
  g_sessions.push_back(s);
}

std::vector<RoutedAppSession> GetRoutedAppSessions() {
  const int bridgePort = GetLocalAuthBridgePort();
  auto clients = EnumBridgeClients(bridgePort);

  // Group bridge clients by friendly app name
  struct Agg {
    std::set<DWORD> pids;
    int conns = 0;
    std::wstring method;
    DWORD primaryPid = 0;
    DWORD startTick = 0;
  };
  std::map<std::wstring, Agg> byName;

  {
    std::lock_guard<std::mutex> lock(g_sessMu);
    // Start from registered launches (name/method known)
    for (const auto& s : g_sessions) {
      auto& a = byName[s.name];
      if (s.pid) a.pids.insert(s.pid);
      if (a.method.empty()) a.method = s.method;
      if (!a.primaryPid) a.primaryPid = s.pid;
      if (!a.startTick) a.startTick = s.startTick;
    }
  }

  // Merge live bridge sockets
  for (const auto& c : clients) {
    std::wstring friendly = FriendlyNameFromImage(c.image);
    // Prefer registered name if PID matches a registration
    {
      std::lock_guard<std::mutex> lock(g_sessMu);
      for (const auto& s : g_sessions) {
        if (s.pid == c.pid) {
          friendly = s.name;
          break;
        }
      }
    }
    auto& a = byName[friendly];
    a.pids.insert(c.pid);
    a.conns += c.conns;
    if (!a.primaryPid) a.primaryPid = c.pid;
  }

  // Keep registered apps visible even before first SOCKS connect, and re-attach
  // multi-process browsers whose launch PID died but children still use the bridge.
  {
    std::lock_guard<std::mutex> lock(g_sessMu);
    for (const auto& s : g_sessions) {
      auto& a = byName[s.name];
      if (a.method.empty()) a.method = s.method;
      if (!a.startTick) a.startTick = s.startTick;
      if (s.pid && PidAlive(s.pid)) {
        a.pids.insert(s.pid);
        if (!a.primaryPid) a.primaryPid = s.pid;
      }
      // Image-name rediscovery: only PIDs that already have bridge connections
      std::wstring img = ImageHintForAppName(s.name);
      if (img.empty()) continue;
      auto pids = FindPidsByImage(img);
      if (s.name == L"RuneLite") {
        auto javas = FindPidsByImage(L"java.exe");
        pids.insert(pids.end(), javas.begin(), javas.end());
      }
      for (DWORD p : pids) {
        bool hasConn = false;
        for (const auto& c : clients) if (c.pid == p) { hasConn = true; break; }
        if (hasConn) {
          a.pids.insert(p);
          if (!a.primaryPid) a.primaryPid = p;
        }
      }
    }
  }

  // Build result list; drop empty ghost apps with no live pids and no conns
  std::vector<RoutedAppSession> result;
  for (auto& kv : byName) {
    Agg& a = kv.second;
    // Drop dead pids
    std::set<DWORD> livePids;
    for (DWORD p : a.pids) {
      if (PidAlive(p)) livePids.insert(p);
    }
    // If we have bridge conns from a pid that failed PidAlive (permissions), still keep conns
    if (livePids.empty() && a.conns == 0) continue;

    RoutedAppSession s;
    s.name = kv.first;
    s.method = a.method.empty() ? L"bridge-client" : a.method;
    s.bridgePort = bridgePort;
    s.startTick = a.startTick;
    s.connCount = a.conns;
    s.processCount = (int)(livePids.empty() ? a.pids.size() : livePids.size());
    s.pid = a.primaryPid;
    if (!livePids.empty() && !livePids.count(s.pid))
      s.pid = *livePids.begin();
    s.alive = true;
    result.push_back(s);
  }

  // Sort by name for stable UI
  std::sort(result.begin(), result.end(), [](const RoutedAppSession& a, const RoutedAppSession& b) {
    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
  });

  // Prune registry of apps with no live processes and not seen for a while
  {
    std::lock_guard<std::mutex> lock(g_sessMu);
    std::vector<RoutedAppSession> keep;
    for (const auto& s : g_sessions) {
      bool still = false;
      for (const auto& r : result) {
        if (_wcsicmp(r.name.c_str(), s.name.c_str()) == 0) { still = true; break; }
      }
      if (still || (GetTickCount() - s.startTick) < 60000)
        keep.push_back(s);
    }
    g_sessions.swap(keep);
  }

  return result;
}

void ClearRoutedAppSessions() {
  std::lock_guard<std::mutex> lock(g_sessMu);
  g_sessions.clear();
}

int TerminateRoutedApp(const std::wstring& appName) {
  if (appName.empty()) return 0;
  std::vector<std::wstring> images;
  if (appName == L"Chrome") images = { L"chrome.exe" };
  else if (appName == L"Edge") images = { L"msedge.exe" };
  else if (appName == L"Brave") images = { L"brave.exe" };
  else if (appName == L"Opera") images = { L"opera.exe" };
  else if (appName == L"Vivaldi") images = { L"vivaldi.exe" };
  else if (appName == L"Firefox") images = { L"firefox.exe" };
  else if (appName == L"Discord") images = { L"Discord.exe" };
  else if (appName == L"Slack") images = { L"slack.exe" };
  else if (appName == L"Teams") images = { L"Teams.exe", L"ms-teams.exe" };
  else if (appName == L"VS Code") images = { L"Code.exe" };
  else if (appName == L"Cursor") images = { L"Cursor.exe" };
  else if (appName == L"Postman") images = { L"Postman.exe" };
  else if (appName == L"Thunderbird") images = { L"thunderbird.exe" };
  else if (appName == L"Spotify") images = { L"Spotify.exe" };
  else if (appName == L"RuneLite") images = { L"RuneLite.exe" };
  else if (appName == L"Jagex") images = { L"JagexLauncher.exe" };
  else images.push_back(appName + L".exe");

  int total = 0;
  {
    std::lock_guard<std::mutex> lock(g_sessMu);
    for (const auto& s : g_sessions) {
      if (_wcsicmp(s.name.c_str(), appName.c_str()) != 0) continue;
      if (!s.pid) continue;
      HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, s.pid);
      if (h) {
        if (TerminateProcess(h, 1)) ++total;
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
      }
    }
  }
  for (const auto& img : images)
    total += TerminateProcessesByImage(img, 4000);

  {
    std::lock_guard<std::mutex> lock(g_sessMu);
    std::vector<RoutedAppSession> keep;
    for (const auto& s : g_sessions) {
      if (_wcsicmp(s.name.c_str(), appName.c_str()) != 0)
        keep.push_back(s);
    }
    g_sessions.swap(keep);
  }
  return total;
}

std::wstring FindDefaultChromePath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = buf;
    p += L"\\Google\\Chrome\\Application\\chrome.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  const wchar_t* candidates[] = {
    L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  };
  for (auto c : candidates) {
    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
  }
  return {};
}

std::wstring FindDefaultEdgePath() {
  const wchar_t* candidates[] = {
    L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
    L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
  };
  for (auto c : candidates) {
    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
  }
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"PROGRAMFILES(X86)", buf, MAX_PATH)) {
    std::wstring p = buf;
    p += L"\\Microsoft\\Edge\\Application\\msedge.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return {};
}

std::wstring FindDefaultFirefoxPath() {
  const wchar_t* candidates[] = {
    L"C:\\Program Files\\Mozilla Firefox\\firefox.exe",
    L"C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe",
  };
  for (auto c : candidates) {
    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
  }
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = buf;
    p += L"\\Mozilla Firefox\\firefox.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return {};
}

static std::wstring BaseNameOf(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

bool IsChromiumLikeExecutable(const std::wstring& exePath) {
  std::wstring b = BaseNameOf(exePath);
  const wchar_t* names[] = {
    L"chrome.exe", L"msedge.exe", L"brave.exe", L"chromium.exe",
    L"opera.exe", L"vivaldi.exe", L"Discord.exe", L"slack.exe",
    L"Teams.exe", L"ms-teams.exe", L"Code.exe", L"Cursor.exe",
    L"Postman.exe", L"Spotify.exe"
  };
  for (auto n : names) {
    if (_wcsicmp(b.c_str(), n) == 0) return true;
  }
  // Electron apps often ship as Product.exe next to resources — allow user pick
  // when path contains common chromium markers
  std::wstring lower = exePath;
  for (auto& c : lower) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
  if (lower.find(L"\\chrome") != std::wstring::npos) return true;
  if (lower.find(L"\\brave") != std::wstring::npos) return true;
  if (lower.find(L"\\discord") != std::wstring::npos) return true;
  return false;
}

// Build full Unicode environment block with SOCKS proxy vars injected (for Electron/Node apps).
// CreateProcessW wants: KEY=VAL\0KEY=VAL\0\0
static std::vector<wchar_t> BuildEnvWithSocksProxy(int localPort) {
  std::wstring proxy = L"socks5://127.0.0.1:" + std::to_wstring(localPort);
  std::wstring noProxy = L"localhost,127.0.0.1,::1";

  // Keys we override (case-sensitive variants used by different tools)
  const wchar_t* overrideKeys[] = {
    L"ALL_PROXY", L"all_proxy", L"HTTP_PROXY", L"http_proxy",
    L"HTTPS_PROXY", L"https_proxy", L"SOCKS_PROXY", L"socks_proxy",
    L"NO_PROXY", L"no_proxy"
  };

  std::vector<std::wstring> lines;
  LPWCH env = GetEnvironmentStringsW();
  if (env) {
    for (LPWCH p = env; *p; ) {
      std::wstring entry = p;
      p += entry.size() + 1;
      size_t eq = entry.find(L'=');
      if (eq == std::wstring::npos) continue;
      std::wstring key = entry.substr(0, eq);
      bool skip = false;
      for (auto ok : overrideKeys) {
        if (_wcsicmp(key.c_str(), ok) == 0) { skip = true; break; }
      }
      if (!skip) lines.push_back(entry);
    }
    FreeEnvironmentStringsW(env);
  }

  lines.push_back(L"ALL_PROXY=" + proxy);
  lines.push_back(L"all_proxy=" + proxy);
  lines.push_back(L"HTTP_PROXY=" + proxy);
  lines.push_back(L"http_proxy=" + proxy);
  lines.push_back(L"HTTPS_PROXY=" + proxy);
  lines.push_back(L"https_proxy=" + proxy);
  lines.push_back(L"SOCKS_PROXY=" + proxy);
  lines.push_back(L"socks_proxy=" + proxy);
  lines.push_back(L"NO_PROXY=" + noProxy);
  lines.push_back(L"no_proxy=" + noProxy);

  std::vector<wchar_t> block;
  for (const auto& line : lines) {
    block.insert(block.end(), line.begin(), line.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

static bool EnsureDir(const std::wstring& path) {
  if (path.empty()) return false;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  // create parents
  for (size_t i = 3; i < path.size(); ++i) {
    if (path[i] == L'\\' || path[i] == L'/') {
      std::wstring sub = path.substr(0, i);
      CreateDirectoryW(sub.c_str(), nullptr);
    }
  }
  return CreateDirectoryW(path.c_str(), nullptr) != 0 ||
         GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// VS Code / Cursor: dedicated user-data-dir with http.proxy (Electron flag alone is not enough)
static std::wstring EnsureIdeProxyUserDataDir(const std::wstring& appTag, int localPort) {
  wchar_t local[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
  std::wstring root = local;
  root += L"\\ProxyPiTester\\Profiles\\";
  root += appTag;
  std::wstring userDir = root + L"\\User";
  if (!EnsureDir(userDir)) return {};

  std::wstring settingsPath = userDir + L"\\settings.json";
  std::string json;
  json += "{\n";
  json += "  \"http.proxy\": \"socks5://127.0.0.1:" + std::to_string(localPort) + "\",\n";
  json += "  \"http.proxySupport\": \"override\",\n";
  json += "  \"http.proxyStrictSSL\": true,\n";
  json += "  \"http.systemCertificates\": true\n";
  json += "}\n";

  HANDLE hF = CreateFileW(settingsPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hF == INVALID_HANDLE_VALUE) return {};
  DWORD wr = 0;
  WriteFile(hF, json.c_str(), (DWORD)json.size(), &wr, nullptr);
  CloseHandle(hF);
  return root;
}

static std::wstring DefaultVscodeExtensionsDir() {
  wchar_t home[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) return {};
  return std::wstring(home) + L"\\.vscode\\extensions";
}

bool LaunchChromiumLikeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                                 const std::wstring& exePath,
                                 const std::wstring& displayName,
                                 bool useTempProfile,
                                 const std::wstring& startUrl) {
  outPid = 0;
  err.clear();

  if (cfg.host.empty() || cfg.port <= 0) {
    err = L"Proxy host/port not configured";
    return false;
  }

  int localPort = 0;
  if (!EnsureLocalAuthBridge(cfg, localPort, err) || localPort <= 0) {
    if (err.empty()) err = L"Failed to start local auth bridge";
    return false;
  }

  if (exePath.empty() || GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"Executable not found: " + exePath;
    return false;
  }

  std::wstring proxyFlag = L"socks5://127.0.0.1:" + std::to_wstring(localPort);
  std::wstring cmdLine = L"\"" + exePath + L"\"";

  const bool isIde = (_wcsicmp(displayName.c_str(), L"VS Code") == 0 ||
                      _wcsicmp(displayName.c_str(), L"Cursor") == 0);
  const bool isElectronApp = !useTempProfile; // Discord, Postman, Slack, Teams, Spotify, IDEs

  if (useTempProfile) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring prof = temp;
    prof += L"ProxyPiTester_";
    prof += displayName;
    prof += L"_";
    prof += std::to_wstring(GetTickCount64());
    CreateDirectoryW(prof.c_str(), nullptr);
    cmdLine += L" --user-data-dir=\"" + prof + L"\"";
    cmdLine += L" --no-first-run --no-default-browser-check";
    cmdLine += L" --disable-features=DnsOverHttps";
  } else if (isIde) {
    // Force VS Code / Cursor proxy via settings.json (Chromium switch alone is ignored)
    std::wstring tag = (_wcsicmp(displayName.c_str(), L"Cursor") == 0) ? L"Cursor" : L"VSCode";
    std::wstring udd = EnsureIdeProxyUserDataDir(tag, localPort);
    if (!udd.empty()) {
      cmdLine += L" --user-data-dir=\"" + udd + L"\"";
      std::wstring ext = DefaultVscodeExtensionsDir();
      if (!ext.empty() && GetFileAttributesW(ext.c_str()) != INVALID_FILE_ATTRIBUTES)
        cmdLine += L" --extensions-dir=\"" + ext + L"\"";
    }
  }

  // Chromium network switch (browsers + some Electron). Electron/Node also need env vars below.
  cmdLine += L" --proxy-server=" + proxyFlag;
  cmdLine += L" --proxy-bypass-list=<-loopback>";
  cmdLine += L" --disable-quic";
  // DNS through proxy for browsers only (can break Electron local IPC if applied broadly)
  if (useTempProfile) {
    cmdLine += L" --host-resolver-rules=\"MAP * ~NOTFOUND , EXCLUDE 127.0.0.1, EXCLUDE localhost\"";
  }

  if (!startUrl.empty()) {
    if (useTempProfile)
      cmdLine += L" --new-window \"" + startUrl + L"\"";
    else
      cmdLine += L" \"" + startUrl + L"\"";
  }

  // Work dir = exe folder (helps Electron find resources)
  std::wstring workDir = exePath;
  size_t slash = workDir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) workDir = workDir.substr(0, slash);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  // Electron / Node apps (Postman, Discord, VS Code) need env proxy vars — CLI alone is not enough
  std::vector<wchar_t> envBlock;
  LPVOID envPtr = nullptr;
  DWORD createFlags = CREATE_UNICODE_ENVIRONMENT;
  if (isElectronApp || isIde) {
    envBlock = BuildEnvWithSocksProxy(localPort);
    envPtr = envBlock.data();
  }

  BOOL ok = CreateProcessW(
    nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
    createFlags, envPtr,
    workDir.empty() ? nullptr : workDir.c_str(),
    &si, &pi);

  if (!ok) {
    err = L"CreateProcess failed. Error=" + std::to_wstring(GetLastError());
    return false;
  }

  outPid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  std::wstring method = useTempProfile ? L"chromium" : (isIde ? L"ide-proxy-settings" : L"electron+env");
  RegisterRoutedApp(outPid, displayName, method, localPort);
  OutputDebugStringW((L"[dev] " + displayName + L" via bridge 127.0.0.1:" +
                      std::to_wstring(localPort) + L" PID=" + std::to_wstring(outPid) +
                      L" method=" + method + L"\r\n").c_str());
  return true;
}

bool LaunchChromeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& chromePath, const std::wstring& startUrl) {
  std::wstring exe = chromePath.empty() ? FindDefaultChromePath() : chromePath;
  if (exe.empty()) {
    err = L"Google Chrome not found. Use Help > App routing to set chrome.exe path.";
    return false;
  }
  std::wstring url = startUrl.empty() ? L"http://ifconfig.me" : startUrl;
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Chrome", true, url);
}

bool LaunchEdgeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                         const std::wstring& edgePath, const std::wstring& startUrl) {
  std::wstring exe = edgePath.empty() ? FindDefaultEdgePath() : edgePath;
  if (exe.empty()) {
    err = L"Microsoft Edge not found (msedge.exe).";
    return false;
  }
  std::wstring url = startUrl.empty() ? L"http://ifconfig.me" : startUrl;
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Edge", true, url);
}

bool LaunchBraveViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& bravePath, const std::wstring& startUrl) {
  std::wstring exe = bravePath.empty() ? FindDefaultBravePath() : bravePath;
  if (exe.empty()) {
    err = L"Brave Browser not found (brave.exe).";
    return false;
  }
  std::wstring url = startUrl.empty() ? L"http://ifconfig.me" : startUrl;
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Brave", true, url);
}

std::wstring FindDefaultBravePath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = buf;
    p += L"\\BraveSoftware\\Brave-Browser\\Application\\brave.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  const wchar_t* candidates[] = {
    L"C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
    L"C:\\Program Files (x86)\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
  };
  for (auto c : candidates) {
    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
  }
  return {};
}

std::wstring FindDefaultDiscordPath() {
  wchar_t buf[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
  std::wstring root = buf;
  root += L"\\Discord";
  // Prefer newest app-1.0.xxxx\Discord.exe
  std::wstring best;
  FILETIME bestWrite{};
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((root + L"\\app-*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
      std::wstring cand = root + L"\\" + fd.cFileName + L"\\Discord.exe";
      if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestWrite) > 0) {
        best = cand;
        bestWrite = fd.ftLastWriteTime;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (!best.empty()) return best;
  std::wstring fallback = root + L"\\Discord.exe";
  if (GetFileAttributesW(fallback.c_str()) != INVALID_FILE_ATTRIBUTES) return fallback;
  return {};
}

bool LaunchDiscordViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& discordPath) {
  std::wstring exe = discordPath.empty() ? FindDefaultDiscordPath() : discordPath;
  if (exe.empty()) {
    err = L"Discord not found under %LOCALAPPDATA%\\Discord\\app-*\\Discord.exe";
    return false;
  }
  // Keep real profile so user stays logged in
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Discord", false, L"");
}

std::wstring FindDefaultOperaPath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Programs\\Opera\\opera.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  const wchar_t* c[] = {
    L"C:\\Program Files\\Opera\\opera.exe",
    L"C:\\Program Files (x86)\\Opera\\opera.exe",
  };
  for (auto x : c) if (GetFileAttributesW(x) != INVALID_FILE_ATTRIBUTES) return x;
  return {};
}

std::wstring FindDefaultVivaldiPath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Vivaldi\\Application\\vivaldi.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  const wchar_t* c[] = {
    L"C:\\Program Files\\Vivaldi\\Application\\vivaldi.exe",
  };
  for (auto x : c) if (GetFileAttributesW(x) != INVALID_FILE_ATTRIBUTES) return x;
  return {};
}

std::wstring FindDefaultSlackPath() {
  wchar_t buf[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
  return FindNewestAppDirExe(std::wstring(buf) + L"\\slack", L"slack.exe");
}

std::wstring FindDefaultTeamsPath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Microsoft\\Teams\\current\\Teams.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    p = std::wstring(buf) + L"\\Microsoft\\WindowsApps\\ms-teams.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return {};
}

std::wstring FindDefaultVSCodePath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Programs\\Microsoft VS Code\\Code.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  const wchar_t* c[] = {
    L"C:\\Program Files\\Microsoft VS Code\\Code.exe",
    L"C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe",
  };
  for (auto x : c) if (GetFileAttributesW(x) != INVALID_FILE_ATTRIBUTES) return x;
  return {};
}

std::wstring FindDefaultCursorPath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Programs\\cursor\\Cursor.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    p = std::wstring(buf) + L"\\Programs\\Cursor\\Cursor.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return {};
}

std::wstring FindDefaultPostmanPath() {
  wchar_t buf[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
  return FindNewestAppDirExe(std::wstring(buf) + L"\\Postman", L"Postman.exe");
}

std::wstring FindDefaultThunderbirdPath() {
  const wchar_t* c[] = {
    L"C:\\Program Files\\Mozilla Thunderbird\\thunderbird.exe",
    L"C:\\Program Files (x86)\\Mozilla Thunderbird\\thunderbird.exe",
  };
  for (auto x : c) if (GetFileAttributesW(x) != INVALID_FILE_ATTRIBUTES) return x;
  return {};
}

std::wstring FindDefaultSpotifyPath() {
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Spotify\\Spotify.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = std::wstring(buf) + L"\\Microsoft\\WindowsApps\\Spotify.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return {};
}

bool LaunchOperaViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& path, const std::wstring& startUrl) {
  std::wstring exe = path.empty() ? FindDefaultOperaPath() : path;
  if (exe.empty()) { err = L"Opera not found (opera.exe)."; return false; }
  std::wstring url = startUrl.empty() ? L"http://ifconfig.me" : startUrl;
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Opera", true, url);
}

bool LaunchVivaldiViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& path, const std::wstring& startUrl) {
  std::wstring exe = path.empty() ? FindDefaultVivaldiPath() : path;
  if (exe.empty()) { err = L"Vivaldi not found (vivaldi.exe)."; return false; }
  std::wstring url = startUrl.empty() ? L"http://ifconfig.me" : startUrl;
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Vivaldi", true, url);
}

bool LaunchSlackViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& path) {
  std::wstring exe = path.empty() ? FindDefaultSlackPath() : path;
  if (exe.empty()) { err = L"Slack not found under %LOCALAPPDATA%\\slack."; return false; }
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Slack", false, L"");
}

bool LaunchTeamsViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& path) {
  std::wstring exe = path.empty() ? FindDefaultTeamsPath() : path;
  if (exe.empty()) { err = L"Microsoft Teams not found."; return false; }
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Teams", false, L"");
}

bool LaunchVSCodeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& path) {
  std::wstring exe = path.empty() ? FindDefaultVSCodePath() : path;
  if (exe.empty()) { err = L"VS Code not found (Code.exe)."; return false; }
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"VS Code", false, L"");
}

bool LaunchCursorViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& path) {
  std::wstring exe = path.empty() ? FindDefaultCursorPath() : path;
  if (exe.empty()) { err = L"Cursor not found (Cursor.exe)."; return false; }
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Cursor", false, L"");
}

bool LaunchPostmanViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& path) {
  std::wstring exe = path.empty() ? FindDefaultPostmanPath() : path;
  if (exe.empty()) { err = L"Postman not found under %LOCALAPPDATA%\\Postman."; return false; }
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Postman", false, L"");
}

bool LaunchSpotifyViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& path) {
  std::wstring exe = path.empty() ? FindDefaultSpotifyPath() : path;
  if (exe.empty()) { err = L"Spotify not found."; return false; }
  // Best-effort Electron-style; Store stubs may ignore proxy flags
  return LaunchChromiumLikeViaBridge(cfg, outPid, err, exe, L"Spotify", false, L"");
}

bool LaunchThunderbirdViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                                const std::wstring& path) {
  outPid = 0;
  err.clear();
  if (cfg.host.empty() || cfg.port <= 0) {
    err = L"Proxy host/port not configured";
    return false;
  }
  int localPort = 0;
  if (!EnsureLocalAuthBridge(cfg, localPort, err) || localPort <= 0) {
    if (err.empty()) err = L"Failed to start local auth bridge";
    return false;
  }
  std::wstring exe = path.empty() ? FindDefaultThunderbirdPath() : path;
  if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"Thunderbird not found (thunderbird.exe).";
    return false;
  }
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring prof = temp;
  prof += L"ProxyPiTester_TB_";
  prof += std::to_wstring(GetTickCount64());
  CreateDirectoryW(prof.c_str(), nullptr);

  std::string prefs;
  prefs += "user_pref(\"network.proxy.type\", 1);\r\n";
  prefs += "user_pref(\"network.proxy.socks\", \"127.0.0.1\");\r\n";
  prefs += "user_pref(\"network.proxy.socks_port\", " + std::to_string(localPort) + ");\r\n";
  prefs += "user_pref(\"network.proxy.socks_version\", 5);\r\n";
  prefs += "user_pref(\"network.proxy.socks_remote_dns\", true);\r\n";
  prefs += "user_pref(\"network.proxy.no_proxies_on\", \"localhost, 127.0.0.1\");\r\n";

  auto writeText = [&](const std::wstring& p, const std::string& body) {
    HANDLE hF = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hF == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(hF, body.c_str(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(hF);
    return ok != FALSE;
  };
  if (!writeText(prof + L"\\user.js", "// ProxyPiTester SOCKS\r\n" + prefs)) {
    err = L"Failed to write Thunderbird user.js";
    return false;
  }
  writeText(prof + L"\\prefs.js", prefs);

  std::wstring cmdLine = L"\"" + exe + L"\" -profile \"" + prof + L"\" -no-remote";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');
  if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi)) {
    err = L"CreateProcess failed for Thunderbird. Error=" + std::to_wstring(GetLastError());
    return false;
  }
  outPid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  RegisterRoutedApp(outPid, L"Thunderbird", L"thunderbird-profile", localPort);
  return true;
}

bool LaunchUrlViaProxiedBrowser(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                                const std::wstring& url) {
  if (url.empty()) {
    err = L"URL is empty";
    return false;
  }
  std::wstring u = url;
  // Allow bare domains
  if (u.find(L"://") == std::wstring::npos)
    u = L"http://" + u;

  if (!FindDefaultChromePath().empty())
    return LaunchChromeViaBridge(cfg, outPid, err, L"", u);
  if (!FindDefaultEdgePath().empty())
    return LaunchEdgeViaBridge(cfg, outPid, err, L"", u);
  if (!FindDefaultBravePath().empty())
    return LaunchBraveViaBridge(cfg, outPid, err, L"", u);
  err = L"No Chromium browser found (Chrome / Edge / Brave) to open the URL.";
  return false;
}

bool LaunchFirefoxViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                            const std::wstring& firefoxPath) {
  outPid = 0;
  err.clear();

  if (cfg.host.empty() || cfg.port <= 0) {
    err = L"Proxy host/port not configured";
    return false;
  }

  int localPort = 0;
  if (!EnsureLocalAuthBridge(cfg, localPort, err) || localPort <= 0) {
    if (err.empty()) err = L"Failed to start local auth bridge";
    return false;
  }

  std::wstring exe = firefoxPath.empty() ? FindDefaultFirefoxPath() : firefoxPath;
  if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"Firefox not found (firefox.exe). Install Mozilla Firefox or set path.";
    return false;
  }

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring prof = temp;
  prof += L"ProxyPiTester_FF_";
  prof += std::to_wstring(GetTickCount64());
  CreateDirectoryW(prof.c_str(), nullptr);

  // Point Firefox at local no-auth bridge; we authenticate upstream.
  std::string prefs;
  prefs += "user_pref(\"network.proxy.type\", 1);\r\n";
  prefs += "user_pref(\"network.proxy.socks\", \"127.0.0.1\");\r\n";
  prefs += "user_pref(\"network.proxy.socks_port\", " + std::to_string(localPort) + ");\r\n";
  prefs += "user_pref(\"network.proxy.socks_version\", 5);\r\n";
  prefs += "user_pref(\"network.proxy.socks_remote_dns\", true);\r\n";
  prefs += "user_pref(\"network.proxy.no_proxies_on\", \"localhost, 127.0.0.1\");\r\n";
  prefs += "user_pref(\"network.proxy.share_proxy_settings\", false);\r\n";
  prefs += "user_pref(\"browser.shell.checkDefaultBrowser\", false);\r\n";
  prefs += "user_pref(\"datareporting.policy.dataSubmissionPolicyBypassNotification\", true);\r\n";

  auto writeText = [&](const std::wstring& path, const std::string& body) {
    HANDLE hF = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hF == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(hF, body.c_str(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(hF);
    return ok != FALSE;
  };

  std::string userJs = "// ProxyPiTester forced SOCKS via local bridge\r\n" + prefs;
  if (!writeText(prof + L"\\user.js", userJs)) {
    err = L"Failed to write Firefox user.js";
    return false;
  }
  writeText(prof + L"\\prefs.js", prefs);
  writeText(prof + L"\\times.json",
            "{\"created\":" + std::to_string((long long)time(nullptr)) + "000}");

  std::wstring cmdLine = L"\"" + exe + L"\" -profile \"" + prof + L"\" -no-remote";
  cmdLine += L" -url http://ifconfig.me";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  BOOL ok = CreateProcessW(
    nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
    CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);

  if (!ok) {
    err = L"CreateProcess failed for Firefox. Error=" + std::to_wstring(GetLastError());
    return false;
  }

  outPid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RegisterRoutedApp(outPid, L"Firefox", L"firefox-profile", localPort);
  OutputDebugStringW((L"[dev] Firefox via bridge 127.0.0.1:" + std::to_wstring(localPort) +
                      L" PID=" + std::to_wstring(outPid) + L"\r\n").c_str());
  return true;
}

std::wstring FindDefaultRuneLiteDir() {
  wchar_t buf[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
  std::wstring dir = buf;
  dir += L"\\RuneLite";
  // Need jar + bundled java at minimum
  std::wstring jar = dir + L"\\RuneLite.jar";
  std::wstring java = dir + L"\\jre\\bin\\java.exe";
  if (GetFileAttributesW(jar.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  if (GetFileAttributesW(java.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
  return dir;
}

bool LaunchRuneLiteViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                             const std::wstring& optionalInstallDir) {
  outPid = 0;
  err.clear();

  if (cfg.host.empty() || cfg.port <= 0) {
    err = L"Proxy host/port not configured";
    return false;
  }

  int localPort = 0;
  if (!EnsureLocalAuthBridge(cfg, localPort, err) || localPort <= 0) {
    if (err.empty()) err = L"Failed to start local auth bridge";
    return false;
  }

  std::wstring dir = optionalInstallDir;
  if (dir.empty()) dir = FindDefaultRuneLiteDir();
  if (dir.empty()) {
    err = L"RuneLite not found. Expected %LOCALAPPDATA%\\RuneLite with jre\\bin\\java.exe and RuneLite.jar";
    return false;
  }

  std::wstring java = dir + L"\\jre\\bin\\java.exe";
  std::wstring jar = dir + L"\\RuneLite.jar";
  std::wstring nativeExe = dir + L"\\RuneLite.exe";
  if (GetFileAttributesW(java.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"RuneLite JRE missing: " + java;
    return false;
  }
  if (GetFileAttributesW(jar.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"RuneLite.jar missing: " + jar;
    return false;
  }

  // Important: RuneLite is two-stage (launcher JVM -> client JVM). SOCKS props on
  // the launcher alone are NOT inherited by the client. Prefer RuneLite.exe with
  // -J <arg> so the native launcher forwards JVM args into the real client.
  // Fallback: java -jar with the same -D flags (still helps launcher HTTP; client
  // may need the -J path).
  //
  // Java SOCKS is no-auth to 127.0.0.1; our bridge authenticates to ProxyPi.
  std::wstring cmdLine;
  if (GetFileAttributesW(nativeExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
    cmdLine = L"\"" + nativeExe + L"\"";
    // -J passes each arg through to the client JVM (RuneLite launcher convention)
    cmdLine += L" -J -DsocksProxyHost=127.0.0.1";
    cmdLine += L" -J -DsocksProxyPort=" + std::to_wstring(localPort);
  } else {
    cmdLine = L"\"" + java + L"\"";
    cmdLine += L" -DsocksProxyHost=127.0.0.1";
    cmdLine += L" -DsocksProxyPort=" + std::to_wstring(localPort);
    cmdLine += L" -XX:+DisableAttachMechanism -Xmx768m -Xss2m";
    cmdLine += L" -jar \"" + jar + L"\"";
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  // Working directory = install dir so relative classPath / resources resolve
  BOOL ok = CreateProcessW(
    nullptr,
    cmdBuf.data(),
    nullptr, nullptr,
    FALSE,
    CREATE_UNICODE_ENVIRONMENT,
    nullptr,
    dir.c_str(),
    &si, &pi);

  if (!ok) {
    err = L"CreateProcess failed for RuneLite. Error=" + std::to_wstring(GetLastError());
    return false;
  }

  outPid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RegisterRoutedApp(outPid, L"RuneLite", L"java-socks", localPort);
  OutputDebugStringW((L"[dev] RuneLite via bridge 127.0.0.1:" + std::to_wstring(localPort) +
                      L" PID=" + std::to_wstring(outPid) + L"\r\n").c_str());
  OutputDebugStringW((L"[dev] cmd: " + cmdLine + L"\r\n").c_str());
  return true;
}

std::wstring FindDefaultJagexLauncherPath() {
  const wchar_t* candidates[] = {
    L"C:\\Program Files (x86)\\Jagex Launcher\\JagexLauncher.exe",
    L"C:\\Program Files\\Jagex Launcher\\JagexLauncher.exe",
  };
  for (auto c : candidates) {
    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
  }
  wchar_t buf[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = buf;
    p += L"\\Jagex Launcher\\JagexLauncher.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return {};
}

bool LaunchJagexViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                          const std::wstring& optionalExePath) {
  outPid = 0;
  err.clear();

  if (cfg.host.empty() || cfg.port <= 0) {
    err = L"Proxy host/port not configured";
    return false;
  }

  int localPort = 0;
  if (!EnsureLocalAuthBridge(cfg, localPort, err) || localPort <= 0) {
    if (err.empty()) err = L"Failed to start local auth bridge";
    return false;
  }

  std::wstring exe = optionalExePath;
  if (exe.empty()) exe = FindDefaultJagexLauncherPath();
  if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"Jagex Launcher not found. Expected Program Files (x86)\\Jagex Launcher\\JagexLauncher.exe";
    return false;
  }

  // CEF (Chromium Embedded Framework) honors the same proxy switch as Chrome.
  // Point at local no-auth bridge; bridge authenticates to ProxyPi.
  std::wstring proxyFlag = L"socks5://127.0.0.1:" + std::to_wstring(localPort);
  std::wstring cmdLine = L"\"" + exe + L"\"";
  cmdLine += L" --proxy-server=\"" + proxyFlag + L"\"";
  cmdLine += L" --disable-quic";

  // Work dir = install folder (CEF resources / locales)
  std::wstring workDir = exe;
  size_t slash = workDir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) workDir = workDir.substr(0, slash);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back(L'\0');

  BOOL ok = CreateProcessW(
    nullptr,
    cmdBuf.data(),
    nullptr, nullptr,
    FALSE,
    CREATE_UNICODE_ENVIRONMENT,
    nullptr,
    workDir.empty() ? nullptr : workDir.c_str(),
    &si, &pi);

  if (!ok) {
    err = L"CreateProcess failed for Jagex Launcher. Error=" + std::to_wstring(GetLastError());
    return false;
  }

  outPid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RegisterRoutedApp(outPid, L"Jagex", L"cef", localPort);
  OutputDebugStringW((L"[dev] Jagex via bridge 127.0.0.1:" + std::to_wstring(localPort) +
                      L" PID=" + std::to_wstring(outPid) + L"\r\n").c_str());
  return true;
}

// ---- RuneLite config.json SOCKS wrap (Jagex Play path) ----

namespace {

std::wstring RuneLiteConfigPath() {
  std::wstring dir = FindDefaultRuneLiteDir();
  if (dir.empty()) {
    // FindDefault requires jar+jre; config may still exist without full install check
    wchar_t buf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
    dir = buf;
    dir += L"\\RuneLite";
  }
  return dir + L"\\config.json";
}

bool ReadFileUtf8(const std::wstring& path, std::string& out, std::wstring& err) {
  out.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = L"Cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  // strip UTF-8 BOM
  if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
      (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
    out.erase(0, 3);
  }
  return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data, std::wstring& err) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = L"Cannot write " + path;
    return false;
  }
  out.write(data.data(), (std::streamsize)data.size());
  if (!out) {
    err = L"Write failed: " + path;
    return false;
  }
  return true;
}

// Remove any vmArgs entries that set socksProxyHost / socksProxyPort (ours or stale).
std::string StripSocksVmArgs(const std::string& json) {
  std::string s = json;
  // Match JSON string entries containing socksProxyHost or socksProxyPort
  // e.g. "    \"-DsocksProxyHost=127.0.0.1\",\r\n"
  auto eraseMatchingLines = [&](const char* needle) {
    for (;;) {
      size_t n = s.find(needle);
      if (n == std::string::npos) break;
      // expand to full quoted entry including trailing comma/whitespace
      size_t q0 = s.rfind('"', n);
      if (q0 == std::string::npos) break;
      // walk back over spaces/tabs before quote (indent)
      size_t lineStart = q0;
      while (lineStart > 0 && (s[lineStart - 1] == ' ' || s[lineStart - 1] == '\t'))
        --lineStart;
      size_t q1 = s.find('"', q0 + 1);
      if (q1 == std::string::npos) break;
      size_t end = q1 + 1;
      if (end < s.size() && s[end] == ',') ++end;
      // eat trailing spaces and one newline
      while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) ++end;
      if (end < s.size() && s[end] == '\r') ++end;
      if (end < s.size() && s[end] == '\n') ++end;
      s.erase(lineStart, end - lineStart);
    }
  };
  eraseMatchingLines("socksProxyHost");
  eraseMatchingLines("socksProxyPort");
  return s;
}

// Insert socks vmArgs just after the opening of "vmArgs": [
bool InsertSocksVmArgs(std::string& json, int port, std::wstring& err) {
  // Find "vmArgs"
  size_t key = json.find("\"vmArgs\"");
  if (key == std::string::npos) {
    err = L"RuneLite config.json has no vmArgs array";
    return false;
  }
  size_t bracket = json.find('[', key);
  if (bracket == std::string::npos) {
    err = L"RuneLite config.json vmArgs is not an array";
    return false;
  }

  // Detect indent from next line if present
  std::string indent = "    ";
  size_t nl = json.find('\n', bracket);
  if (nl != std::string::npos && nl + 1 < json.size()) {
    size_t i = nl + 1;
    size_t start = i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i > start) indent = json.substr(start, i - start);
  }

  std::string insert;
  insert += "\n";
  insert += indent;
  insert += "\"-DsocksProxyHost=127.0.0.1\",\n";
  insert += indent;
  insert += "\"-DsocksProxyPort=";
  insert += std::to_string(port);
  insert += "\",";

  json.insert(bracket + 1, insert);
  return true;
}

int ParseSocksPortFromConfig(const std::string& json) {
  size_t n = json.find("socksProxyPort=");
  if (n == std::string::npos) {
    n = json.find("socksProxyPort\\u003d");
    if (n == std::string::npos) return 0;
    n += sizeof("socksProxyPort\\u003d") - 1;
  } else {
    n += sizeof("socksProxyPort=") - 1;
  }
  int port = 0;
  while (n < json.size() && json[n] >= '0' && json[n] <= '9') {
    port = port * 10 + (json[n] - '0');
    ++n;
  }
  return port;
}

} // namespace

bool ApplyRuneLiteSocksWrap(int localSocksPort, std::wstring& err) {
  err.clear();
  if (localSocksPort <= 0 || localSocksPort > 65535) {
    err = L"Invalid local SOCKS port for RuneLite wrap";
    return false;
  }
  std::wstring path = RuneLiteConfigPath();
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"RuneLite config.json not found (install RuneLite first)";
    return false;
  }

  std::wstring bak = path + L".proxypi.bak";
  if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) {
    CopyFileW(path.c_str(), bak.c_str(), TRUE);
  }

  std::string json;
  if (!ReadFileUtf8(path, json, err)) return false;
  json = StripSocksVmArgs(json);
  if (!InsertSocksVmArgs(json, localSocksPort, err)) return false;
  if (!WriteFileUtf8(path, json, err)) return false;

  g_runeLiteWrapDesired = true;
  OutputDebugStringW((L"[dev] RuneLite SOCKS wrap applied port=" +
                      std::to_wstring(localSocksPort) + L"\r\n").c_str());
  return true;
}

bool ClearRuneLiteSocksWrap(std::wstring& err) {
  err.clear();
  g_runeLiteWrapDesired = false;
  std::wstring path = RuneLiteConfigPath();
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  std::string json;
  if (!ReadFileUtf8(path, json, err)) return false;
  if (json.find("socksProxyHost") == std::string::npos &&
      json.find("socksProxyPort") == std::string::npos) {
    return true;
  }
  json = StripSocksVmArgs(json);
  if (!WriteFileUtf8(path, json, err)) return false;
  OutputDebugStringW(L"[dev] RuneLite SOCKS wrap cleared\r\n");
  return true;
}

bool IsRuneLiteSocksWrapActive() {
  std::wstring path = RuneLiteConfigPath();
  if (path.empty()) return false;
  std::string json;
  std::wstring err;
  if (!ReadFileUtf8(path, json, err)) return false;
  return json.find("socksProxyHost") != std::string::npos &&
         json.find("socksProxyPort") != std::string::npos;
}

int GetRuneLiteSocksWrapPort() {
  std::wstring path = RuneLiteConfigPath();
  if (path.empty()) return 0;
  std::string json;
  std::wstring err;
  if (!ReadFileUtf8(path, json, err)) return 0;
  return ParseSocksPortFromConfig(json);
}

void SetRuneLiteWrapDesired(bool desired) {
  g_runeLiteWrapDesired = desired;
}

bool IsRuneLiteWrapDesired() {
  return g_runeLiteWrapDesired.load();
}

bool ReArmRuneLiteWrapIfNeeded(std::wstring& err) {
  err.clear();
  if (!g_runeLiteWrapDesired.load()) return true;
  if (IsRuneLiteSocksWrapActive()) {
    // Port may be stale if bridge restarted — refresh if port mismatch
    int want = GetLocalAuthBridgePort();
    int have = GetRuneLiteSocksWrapPort();
    if (want <= 0 || want == have) return true;
  }
  int port = GetLocalAuthBridgePort();
  if (port <= 0) {
    if (!EnsureBridgeAlive(err)) return false;
    port = GetLocalAuthBridgePort();
  }
  if (port <= 0) {
    err = L"No bridge port for RuneLite wrap re-arm";
    return false;
  }
  return ApplyRuneLiteSocksWrap(port, err);
}

bool LaunchJagexWithRuneLiteWrap(const ProxyConfig& cfg, DWORD& outPid, int& outBridgePort,
                                 std::wstring& err, const std::wstring& optionalExePath) {
  outPid = 0;
  outBridgePort = 0;
  err.clear();

  if (!EnsureLocalAuthBridge(cfg, outBridgePort, err) || outBridgePort <= 0) {
    if (err.empty()) err = L"Failed to start local auth bridge";
    return false;
  }

  if (!ApplyRuneLiteSocksWrap(outBridgePort, err)) {
    // Still launch Jagex? Better fail clearly so user knows wrap didn't arm.
    return false;
  }

  if (!LaunchJagexViaBridge(cfg, outPid, err, optionalExePath)) {
    // Leave wrap armed if bridge is up - user may retry Play.
    return false;
  }
  return true;
}

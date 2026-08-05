#include "local_bridge.h"
#include "proxy_client.h"
#include "common.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace {

std::mutex g_mu;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
SOCKET g_listenSock = INVALID_SOCKET;
int g_localPort = 0;
ProxyConfig g_cfg;
std::thread g_acceptThread;
std::wstring g_lastErr;

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
  Log(L"[bridge] acceptor started on 127.0.0.1:" + std::to_wstring(g_localPort));

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

  Log(L"[bridge] acceptor stopped");
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
      outLocalPort = g_localPort;
      portForLog = g_localPort;
      reused = true;
      hostForLog = cfg.host;
      upPortForLog = cfg.port;
      authForLog = cfg.useAuth;
    } else {
      g_stop = false;
      g_cfg = cfg;

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

  if (reused) {
    Log(L"[bridge] reusing 127.0.0.1:" + std::to_wstring(portForLog) +
        L" -> " + hostForLog + L":" + std::to_wstring(upPortForLog));
  } else {
    Log(L"[bridge] listening on 127.0.0.1:" + std::to_wstring(portForLog) +
        L" -> upstream " + hostForLog + L":" + std::to_wstring(upPortForLog) +
        (authForLog ? L" (with auth)" : L""));
  }
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

static std::wstring FindChromeExe() {
  wchar_t buf[MAX_PATH] = {};
  // User install (most common)
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
    std::wstring p = buf;
    p += L"\\Google\\Chrome\\Application\\chrome.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  // System install
  const wchar_t* candidates[] = {
    L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  };
  for (auto c : candidates) {
    if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
  }
  return {};
}

bool LaunchChromeViaBridge(const ProxyConfig& cfg, DWORD& outPid, std::wstring& err,
                           const std::wstring& chromePath) {
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

  std::wstring exe = chromePath;
  if (exe.empty()) exe = FindChromeExe();
  if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"Google Chrome not found. Install Chrome or set path manually.";
    return false;
  }

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring prof = temp;
  prof += L"ProxyPiTester_Chrome_";
  prof += std::to_wstring(GetTickCount64());
  CreateDirectoryW(prof.c_str(), nullptr);

  std::wstring proxyFlag = L"socks5://127.0.0.1:" + std::to_wstring(localPort);
  std::wstring cmdLine = L"\"" + exe + L"\"";
  cmdLine += L" --user-data-dir=\"" + prof + L"\"";
  cmdLine += L" --proxy-server=\"" + proxyFlag + L"\"";
  cmdLine += L" --no-first-run --no-default-browser-check --disable-quic";
  cmdLine += L" --disable-features=DnsOverHttps";
  cmdLine += L" --new-window http://ifconfig.me";

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
    nullptr,
    &si, &pi);

  if (!ok) {
    err = L"CreateProcess failed. Error=" + std::to_wstring(GetLastError());
    return false;
  }

  outPid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  Log(L"[dev] Chrome launched via bridge 127.0.0.1:" + std::to_wstring(localPort) +
      L" PID=" + std::to_wstring(outPid));
  Log(L"[dev] Profile: " + prof);
  Log(L"[dev] Open ifconfig.me — should show proxy exit IP.");
  return true;
}

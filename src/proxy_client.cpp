#include "proxy_client.h"
#include "common.h"
#include <chrono>
#include <vector>
#include <algorithm>
#pragma comment(lib, "ws2_32.lib")

static bool g_winsockInit = false;

bool InitWinsock(std::wstring& err) {
  if (g_winsockInit) return true;
  WSADATA wsa;
  int res = WSAStartup(MAKEWORD(2, 2), &wsa);
  if (res != 0) {
    err = L"WSAStartup failed: " + std::to_wstring(res);
    return false;
  }
  g_winsockInit = true;
  return true;
}

std::string ProxyConfig::hostA() const {
  int len = WideCharToMultiByte(CP_UTF8, 0, host.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, host.c_str(), -1, &out[0], len, nullptr, nullptr);
  if (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

std::string ProxyConfig::usernameA() const {
  int len = WideCharToMultiByte(CP_UTF8, 0, username.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, username.c_str(), -1, &out[0], len, nullptr, nullptr);
  if (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

std::string ProxyConfig::passwordA() const {
  int len = WideCharToMultiByte(CP_UTF8, 0, password.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, password.c_str(), -1, &out[0], len, nullptr, nullptr);
  if (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

bool ConnectTcp(const std::string& host, int port, SOCKET& outSock, std::wstring& err) {
  outSock = INVALID_SOCKET;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  char portStr[16];
  _itoa_s(port, portStr, 10);

  addrinfo* result = nullptr;
  int gai = getaddrinfo(host.c_str(), portStr, &hints, &result);
  if (gai != 0) {
    err = L"DNS resolution failed for " + std::wstring(host.begin(), host.end());
    return false;
  }

  SOCKET s = INVALID_SOCKET;
  for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
    s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (s == INVALID_SOCKET) continue;

    if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) {
      outSock = s;
      freeaddrinfo(result);
      return true;
    }
    closesocket(s);
    s = INVALID_SOCKET;
  }
  freeaddrinfo(result);
  err = L"Failed to connect to " + std::wstring(host.begin(), host.end()) + L":" + std::to_wstring(port);
  return false;
}

// ---- SOCKS5 ----

static bool Socks5Auth(SOCKET s, const ProxyConfig& cfg, std::wstring& err) {
  if (!cfg.useAuth) return true;

  std::string user = cfg.usernameA();
  std::string pass = cfg.passwordA();

  unsigned char buf[512];
  size_t idx = 0;
  buf[idx++] = 0x01;                 // subnegotiation version
  buf[idx++] = (unsigned char)std::min<size_t>(255, user.size());
  memcpy(&buf[idx], user.data(), user.size());
  idx += user.size();
  buf[idx++] = (unsigned char)std::min<size_t>(255, pass.size());
  memcpy(&buf[idx], pass.data(), pass.size());
  idx += pass.size();

  if (send(s, (char*)buf, (int)idx, 0) != (int)idx) {
    err = L"SOCKS5 auth send failed";
    return false;
  }

  unsigned char resp[2];
  if (recv(s, (char*)resp, 2, 0) != 2) {
    err = L"SOCKS5 auth recv failed";
    return false;
  }
  if (resp[0] != 0x01 || resp[1] != 0x00) {
    err = L"SOCKS5 authentication failed (wrong username/password or unsupported)";
    return false;
  }
  return true;
}

bool Socks5Connect(SOCKET s, const ProxyConfig& cfg,
                   const std::string& targetHost, int targetPort,
                   std::wstring& err) {
  // Greeting
  unsigned char greet[3] = { 0x05, 0x01, (unsigned char)(cfg.useAuth ? 0x02 : 0x00) };
  if (send(s, (char*)greet, 3, 0) != 3) {
    err = L"SOCKS5 greeting send failed";
    return false;
  }

  unsigned char greetResp[2];
  if (recv(s, (char*)greetResp, 2, 0) != 2) {
    err = L"SOCKS5 greeting recv failed";
    return false;
  }
  if (greetResp[0] != 0x05) {
    err = L"Invalid SOCKS5 response";
    return false;
  }
  unsigned char method = greetResp[1];

  if (method == 0xFF) {
    err = L"SOCKS5: no acceptable authentication method";
    return false;
  }
  if (cfg.useAuth && method != 0x02) {
    err = L"SOCKS5: server did not offer username/password auth";
    return false;
  }
  if (!cfg.useAuth && method != 0x00) {
    // Some servers still ask for auth even if we offered none. Try to proceed.
  }

  if (method == 0x02) {
    if (!Socks5Auth(s, cfg, err)) return false;
  }

  // CONNECT request
  unsigned char req[512];
  size_t idx = 0;
  req[idx++] = 0x05; // VER
  req[idx++] = 0x01; // CMD = CONNECT
  req[idx++] = 0x00; // RSV

  in6_addr v6{};
  in_addr v4{};
  if (inet_pton(AF_INET6, targetHost.c_str(), &v6) == 1) {
    req[idx++] = 0x04; // ATYP IPv6
    memcpy(&req[idx], &v6, 16);
    idx += 16;
  } else if (inet_pton(AF_INET, targetHost.c_str(), &v4) == 1) {
    req[idx++] = 0x01; // ATYP IPv4
    memcpy(&req[idx], &v4, 4);
    idx += 4;
  } else {
    // Domain name (best for DNS through proxy too)
    req[idx++] = 0x03; // ATYP DOMAINNAME
    size_t hlen = std::min<size_t>(255, targetHost.size());
    req[idx++] = (unsigned char)hlen;
    memcpy(&req[idx], targetHost.data(), hlen);
    idx += hlen;
  }

  unsigned short netPort = htons((unsigned short)targetPort);
  memcpy(&req[idx], &netPort, 2);
  idx += 2;

  if (send(s, (char*)req, (int)idx, 0) != (int)idx) {
    err = L"SOCKS5 CONNECT request send failed";
    return false;
  }

  // Reply
  unsigned char rep[4];
  if (recv(s, (char*)rep, 4, 0) != 4) {
    err = L"SOCKS5 CONNECT reply recv failed";
    return false;
  }
  if (rep[0] != 0x05 || rep[1] != 0x00) {
    char code = rep[1];
    err = L"SOCKS5 CONNECT failed, reply code: " + std::to_wstring((int)code);
    return false;
  }

  // Consume the rest of the reply (BND.ADDR + BND.PORT)
  unsigned char atyp = rep[3];
  size_t toRead = 0;
  if (atyp == 0x01) toRead = 4;       // IPv4
  else if (atyp == 0x03) {
    unsigned char len;
    if (recv(s, (char*)&len, 1, 0) != 1) { err = L"SOCKS5 BND read failed"; return false; }
    toRead = len;
  } else if (atyp == 0x04) toRead = 16; // IPv6
  else { err = L"SOCKS5 unknown ATYP in reply"; return false; }

  unsigned char rest[256];
  if (toRead > 0 && recv(s, (char*)rest, (int)toRead, 0) != (int)toRead) {
    err = L"SOCKS5 failed to read bound address";
    return false;
  }
  unsigned char portBuf[2];
  if (recv(s, (char*)portBuf, 2, 0) != 2) {
    err = L"SOCKS5 failed to read bound port";
    return false;
  }

  return true;
}

// ---- HTTP Proxy ----

static std::string Base64Encode(const std::string& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    unsigned int n = ((unsigned char)in[i]) << 16;
    if (i + 1 < in.size()) n |= ((unsigned char)in[i + 1]) << 8;
    if (i + 2 < in.size()) n |= (unsigned char)in[i + 2];
    out.push_back(tbl[(n >> 18) & 63]);
    out.push_back(tbl[(n >> 12) & 63]);
    out.push_back(i + 1 < in.size() ? tbl[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < in.size() ? tbl[n & 63] : '=');
  }
  return out;
}

bool HttpProxyConnect(SOCKET s, const ProxyConfig& cfg,
                      const std::string& targetHost, int targetPort,
                      std::wstring& err) {
  std::string req;
  req = "CONNECT " + targetHost + ":" + std::to_string(targetPort) + " HTTP/1.1\r\n";
  req += "Host: " + targetHost + ":" + std::to_string(targetPort) + "\r\n";
  req += "Proxy-Connection: keep-alive\r\n";

  if (cfg.useAuth) {
    std::string creds = cfg.usernameA() + ":" + cfg.passwordA();
    req += "Proxy-Authorization: Basic " + Base64Encode(creds) + "\r\n";
  }
  req += "\r\n";

  if (send(s, req.data(), (int)req.size(), 0) != (int)req.size()) {
    err = L"HTTP proxy CONNECT send failed";
    return false;
  }

  // Read response until \r\n\r\n
  std::string resp;
  char buf[1024];
  auto start = std::chrono::steady_clock::now();
  while (resp.find("\r\n\r\n") == std::string::npos) {
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      err = L"HTTP proxy CONNECT response recv failed";
      return false;
    }
    buf[n] = 0;
    resp.append(buf, n);
    if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() > 15) {
      err = L"HTTP proxy CONNECT timed out";
      return false;
    }
  }

  // Robust status check: look for " 200 " after the HTTP version line
  bool is200 = false;
  size_t httpPos = resp.find("HTTP/");
  if (httpPos != std::string::npos) {
    size_t space = resp.find(' ', httpPos);
    if (space != std::string::npos && space + 4 <= resp.size()) {
      std::string code = resp.substr(space + 1, 3);
      if (code == "200") is200 = true;
    }
  }
  if (!is200) {
    // Extract first line for error
    size_t lineEnd = resp.find("\r\n");
    std::string firstLine = (lineEnd != std::string::npos) ? resp.substr(0, lineEnd) : resp;
    err = L"HTTP proxy CONNECT failed: " + std::wstring(firstLine.begin(), firstLine.end());
    return false;
  }

  return true;
}

bool SendHttpGet(SOCKET s, const std::string& host, const std::string& path,
                 std::string& outBody, std::wstring& err) {
  std::string req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: ProxyTools/1.0\r\n";
  req += "Connection: close\r\n\r\n";

  if (send(s, req.data(), (int)req.size(), 0) != (int)req.size()) {
    err = L"HTTP GET send failed";
    return false;
  }

  // Read headers until \r\n\r\n
  std::string headers;
  char buf[4096];
  bool headersDone = false;
  while (!headersDone) {
    int n = recv(s, buf, sizeof(buf), 0);
    if (n <= 0) {
      err = L"HTTP response recv failed while reading headers";
      return false;
    }
    headers.append(buf, n);
    if (headers.find("\r\n\r\n") != std::string::npos) {
      headersDone = true;
    }
    if (headers.size() > 16 * 1024) break; // safety
  }

  size_t sep = headers.find("\r\n\r\n");
  if (sep == std::string::npos) {
    outBody = headers;
    return true;
  }

  std::string headerPart = headers.substr(0, sep);
  outBody = headers.substr(sep + 4);

  // Try to respect Content-Length so we don't wait for close (case-insensitive)
  std::string lowerHeader = headerPart;
  std::transform(lowerHeader.begin(), lowerHeader.end(), lowerHeader.begin(), [](unsigned char c){ return std::tolower(c); });
  size_t clPos = lowerHeader.find("content-length:");
  if (clPos != std::string::npos) {
    // Find the corresponding position in original for parsing
    size_t origClPos = headerPart.find(":", clPos); // rough, but since same length
    if (origClPos != std::string::npos) {
      size_t valStart = headerPart.find_first_not_of(" \t", origClPos + 1);
      if (valStart != std::string::npos) {
        size_t valEnd = headerPart.find_first_of("\r\n", valStart);
        std::string lenStr = headerPart.substr(valStart, valEnd - valStart);
        // trim lenStr
        lenStr.erase(0, lenStr.find_first_not_of(" \t"));
        lenStr.erase(lenStr.find_last_not_of(" \t\r\n") + 1);
        int contentLen = std::atoi(lenStr.c_str());
        if (contentLen > 0) {
          // Read exactly the remaining body bytes if needed
          while ((int)outBody.size() < contentLen) {
            int need = contentLen - (int)outBody.size();
            int n = recv(s, buf, (need > sizeof(buf) ? sizeof(buf) : need), 0);
            if (n <= 0) break;
            outBody.append(buf, n);
          }
        }
      }
    }
  } else {
    // No Content-Length (e.g. chunked or close-delimited) - read until close or safety
    for (;;) {
      int n = recv(s, buf, sizeof(buf), 0);
      if (n <= 0) break;
      outBody.append(buf, n);
      if (outBody.size() > 64 * 1024) break;
    }
  }

  return true;
}

ProxyTestResult TestProxy(const ProxyConfig& cfg, const std::string& testHost, int testPort) {
  ProxyTestResult res;
  std::wstring err;
  if (!InitWinsock(err)) {
    res.message = err;
    return res;
  }

  auto t0 = std::chrono::steady_clock::now();

  SOCKET proxySock = INVALID_SOCKET;
  if (!ConnectTcp(cfg.hostA(), cfg.port, proxySock, err)) {
    res.message = err;
    return res;
  }

  // Set a receive timeout so we don't hang forever on slow proxies
  int recvTimeoutMs = 10000; // 10 seconds
  setsockopt(proxySock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));

  bool tunnelOk = false;
  if (cfg.type == ProxyType::SOCKS5) {
    tunnelOk = Socks5Connect(proxySock, cfg, testHost, testPort, err);
  } else {
    tunnelOk = HttpProxyConnect(proxySock, cfg, testHost, testPort, err);
  }

  if (!tunnelOk) {
    closesocket(proxySock);
    res.message = err;
    return res;
  }

  // Now send HTTP request to the target through the tunnel
  std::string body;
  if (!SendHttpGet(proxySock, testHost, "/", body, err)) {
    closesocket(proxySock);
    res.message = err;
    return res;
  }

  closesocket(proxySock);

  auto t1 = std::chrono::steady_clock::now();
  res.latencyMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  // Clean body (trim + take first non-empty line)
  size_t start = body.find_first_not_of(" \t\r\n");
  if (start != std::string::npos) body = body.substr(start);
  size_t nl = body.find_first_of("\r\n");
  if (nl != std::string::npos) body = body.substr(0, nl);
  size_t end = body.find_last_not_of(" \t\r\n");
  if (end != std::string::npos) body = body.substr(0, end + 1);

  res.detectedIp = std::wstring(body.begin(), body.end());
  res.success = !res.detectedIp.empty() && res.detectedIp.find(L'.') != std::wstring::npos;
  if (res.success) {
    res.message = L"Success";
  } else {
    res.message = L"Got response but could not parse IP: " + std::wstring(body.begin(), body.end());
  }
  return res;
}

// Stream an HTTP GET body through an open tunnel; count bytes only (no full buffer).
static bool DownloadHttpGetCount(SOCKET s, const std::string& host, const std::string& path,
                                 size_t maxBytes, size_t& outBytes, std::wstring& err) {
  outBytes = 0;
  std::string req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: ProxyPi-Tester/1.0\r\n";
  req += "Connection: close\r\n\r\n";

  if (send(s, req.data(), (int)req.size(), 0) != (int)req.size()) {
    err = L"Speed test HTTP GET send failed";
    return false;
  }

  std::string headers;
  char buf[8192];
  bool headersDone = false;

  auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);

  while (!headersDone) {
    if (std::chrono::steady_clock::now() > tDeadline) {
      err = L"Speed test timed out reading headers";
      return false;
    }
    int n = recv(s, buf, sizeof(buf), 0);
    if (n <= 0) {
      err = L"Speed test failed reading HTTP headers";
      return false;
    }
    headers.append(buf, n);
    size_t sep = headers.find("\r\n\r\n");
    if (sep != std::string::npos) {
      headersDone = true;
      // Any body bytes already received after headers
      size_t bodyInBuf = headers.size() - (sep + 4);
      if (bodyInBuf > 0) {
        outBytes += bodyInBuf;
      }
      // Keep only the header part for status check
      headers.resize(sep);
    }
    if (headers.size() > 32 * 1024) {
      err = L"Speed test: HTTP headers too large";
      return false;
    }
  }

  // Status line check
  bool is200 = false;
  size_t httpPos = headers.find("HTTP/");
  if (httpPos != std::string::npos) {
    size_t space = headers.find(' ', httpPos);
    if (space != std::string::npos && space + 4 <= headers.size()) {
      if (headers.substr(space + 1, 3) == "200") is200 = true;
    }
  }
  if (!is200) {
    size_t lineEnd = headers.find("\r\n");
    std::string first = (lineEnd != std::string::npos) ? headers.substr(0, lineEnd) : headers;
    err = L"Speed test HTTP failed: " + std::wstring(first.begin(), first.end());
    return false;
  }

  // Stream remaining body up to maxBytes
  while (outBytes < maxBytes) {
    if (std::chrono::steady_clock::now() > tDeadline) {
      // Partial download still usable for speed estimate
      break;
    }
    int want = (int)std::min<size_t>(sizeof(buf), maxBytes - outBytes);
    int n = recv(s, buf, want, 0);
    if (n <= 0) break;
    outBytes += (size_t)n;
  }

  return outBytes > 0;
}

SpeedTestResult SpeedTestProxy(const ProxyConfig& cfg) {
  SpeedTestResult res;
  std::wstring err;
  if (!InitWinsock(err)) {
    res.message = err;
    return res;
  }

  // Phase 1: quick IP + latency check (reuses existing tester)
  auto t0 = std::chrono::steady_clock::now();
  ProxyTestResult ipTest = TestProxy(cfg, "api.ipify.org", 80);
  auto t1 = std::chrono::steady_clock::now();
  res.latencyMs = ipTest.latencyMs > 0
    ? ipTest.latencyMs
    : (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  res.detectedIp = ipTest.detectedIp;

  if (!ipTest.success) {
    res.message = L"Connectivity/IP check failed: " + ipTest.message;
    return res;
  }

  // Phase 2: download a known payload through the proxy for throughput
  // Prefer a modest 1MB file so tests finish quickly on residential links.
  const std::string dlHost = "speedtest.tele2.net";
  const std::string dlPath = "/1MB.zip";
  const int dlPort = 80;
  const size_t targetBytes = 1024 * 1024; // stop after ~1 MB

  SOCKET proxySock = INVALID_SOCKET;
  if (!ConnectTcp(cfg.hostA(), cfg.port, proxySock, err)) {
    res.message = L"IP OK, but speed test connect failed: " + err;
    // Still report partial success with IP
    res.success = true;
    res.downloadMbps = 0;
    return res;
  }

  int recvTimeoutMs = 20000;
  setsockopt(proxySock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeoutMs, sizeof(recvTimeoutMs));
  int sendTimeoutMs = 15000;
  setsockopt(proxySock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeoutMs, sizeof(sendTimeoutMs));

  bool tunnelOk = false;
  if (cfg.type == ProxyType::SOCKS5) {
    tunnelOk = Socks5Connect(proxySock, cfg, dlHost, dlPort, err);
  } else {
    tunnelOk = HttpProxyConnect(proxySock, cfg, dlHost, dlPort, err);
  }
  if (!tunnelOk) {
    closesocket(proxySock);
    res.message = L"IP OK, speed tunnel failed: " + err;
    res.success = true;
    return res;
  }

  auto td0 = std::chrono::steady_clock::now();
  size_t bytes = 0;
  if (!DownloadHttpGetCount(proxySock, dlHost, dlPath, targetBytes, bytes, err)) {
    closesocket(proxySock);
    res.message = L"IP OK, download failed: " + err;
    res.success = true;
    return res;
  }
  auto td1 = std::chrono::steady_clock::now();
  closesocket(proxySock);

  res.bytesDownloaded = bytes;
  res.downloadMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(td1 - td0).count();
  if (res.downloadMs < 1) res.downloadMs = 1;

  // Mbps = (bytes * 8) / (ms / 1000) / 1_000_000 = bytes * 8 / ms / 1000
  res.downloadMbps = (double)bytes * 8.0 / (double)res.downloadMs / 1000.0;
  res.success = true;

  wchar_t speedBuf[64];
  swprintf_s(speedBuf, L"%.2f", res.downloadMbps);
  res.message = L"Speed test complete - " + std::wstring(speedBuf) + L" Mbps down";
  return res;
}
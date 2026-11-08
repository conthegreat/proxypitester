#include "proxy_client.h"
#include "common.h"
#include <chrono>
#include <vector>
#include <algorithm>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

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

  unsigned char buf[600];
  size_t idx = 0;
  size_t ulen = std::min<size_t>(255, user.size());
  size_t plen = std::min<size_t>(255, pass.size());
  buf[idx++] = 0x01;                 // subnegotiation version
  buf[idx++] = (unsigned char)ulen;
  if (ulen) memcpy(&buf[idx], user.data(), ulen);
  idx += ulen;
  buf[idx++] = (unsigned char)plen;
  if (plen) memcpy(&buf[idx], pass.data(), plen);
  idx += plen;

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

// Public 100 MB payload. GitHub 302s this to release-assets.githubusercontent.com.
static const char* kSpeedTestUrl =
    "https://github.com/conthegreat/speedtest/releases/download/100mb-test/100mb.bin";
static const int kSpeedSampleMs = 10000;
static const size_t kSpeedMinBytes = 64 * 1024;
// Stop at 20 MB or 10 seconds. The file is 100 MB; a full pull would burn proxy quota.
static const size_t kSpeedMaxBytes = 20ull * 1024ull * 1024ull;

static bool SendAllSock(SOCKET s, const char* data, int n) {
  int sent = 0;
  while (sent < n) {
    int r = send(s, data + sent, n - sent, 0);
    if (r <= 0) return false;
    sent += r;
  }
  return true;
}

struct HttpsUrl {
  std::string host;
  std::string path; // includes query, starts with /
  int port = 443;
};

static bool ParseHttpsUrl(const std::string& url, HttpsUrl& out) {
  const std::string pfx = "https://";
  if (url.size() <= pfx.size() || url.compare(0, pfx.size(), pfx) != 0) return false;
  std::string rest = url.substr(pfx.size());
  size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  out.path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
  if (hostport.empty() || out.path.empty()) return false;
  size_t colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
    out.host = hostport.substr(0, colon);
    out.port = atoi(hostport.c_str() + colon + 1);
    if (out.port <= 0) out.port = 443;
  } else {
    out.host = hostport;
    out.port = 443;
  }
  return !out.host.empty();
}

// One TLS session over an already-open proxy tunnel. Single threaded on purpose:
// the previous WinHTTP local-proxy design spawned extra threads and could take
// the process down with them.
struct TlsConn {
  SOCKET sock = INVALID_SOCKET;
  CredHandle cred{};
  CtxtHandle ctx{};
  bool hasCred = false;
  bool hasCtx = false;
  SecPkgContext_StreamSizes sizes{};
  std::vector<char> enc;
  std::vector<char> plain;

  ~TlsConn() {
    if (hasCtx) DeleteSecurityContext(&ctx);
    if (hasCred) FreeCredentialsHandle(&cred);
  }
};

static bool TlsHandshake(TlsConn& t, const std::string& host, std::wstring& err) {
  SCHANNEL_CRED sc{};
  sc.dwVersion = SCHANNEL_CRED_VERSION;
  sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;

  TimeStamp exp{};
  SECURITY_STATUS ss = AcquireCredentialsHandleA(
      nullptr, const_cast<SEC_CHAR*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND,
      nullptr, &sc, nullptr, nullptr, &t.cred, &exp);
  if (ss != SEC_E_OK) {
    err = L"TLS init failed (" + std::to_wstring((long)ss) + L")";
    return false;
  }
  t.hasCred = true;

  const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
  std::vector<char> incoming;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

  for (;;) {
    if (std::chrono::steady_clock::now() > deadline) {
      err = L"TLS handshake timed out";
      return false;
    }

    SecBuffer ib[2]{};
    ib[0].BufferType = SECBUFFER_TOKEN;
    ib[0].pvBuffer = incoming.empty() ? nullptr : incoming.data();
    ib[0].cbBuffer = (ULONG)incoming.size();
    ib[1].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, ib };

    SecBuffer ob[1]{};
    ob[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, ob };
    DWORD outFlags = 0;

    ss = InitializeSecurityContextA(
        &t.cred,
        t.hasCtx ? &t.ctx : nullptr,
        t.hasCtx ? nullptr : const_cast<SEC_CHAR*>(host.c_str()),
        flags, 0, 0,
        incoming.empty() ? nullptr : &inDesc,
        0,
        t.hasCtx ? nullptr : &t.ctx,
        &outDesc,
        &outFlags,
        &exp);

    if (!t.hasCtx && (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK)) t.hasCtx = true;

    if (ob[0].cbBuffer && ob[0].pvBuffer) {
      bool sent = SendAllSock(t.sock, (const char*)ob[0].pvBuffer, (int)ob[0].cbBuffer);
      FreeContextBuffer(ob[0].pvBuffer);
      if (!sent) {
        err = L"TLS handshake send failed";
        return false;
      }
    }

    if (ss == SEC_E_OK) {
      if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer > 0 &&
          ib[1].cbBuffer <= incoming.size()) {
        size_t extra = ib[1].cbBuffer;
        t.enc.assign(incoming.end() - (ptrdiff_t)extra, incoming.end());
      }
      break;
    }

    if (ss == SEC_E_INCOMPLETE_MESSAGE || ss == SEC_I_CONTINUE_NEEDED) {
      if (ss == SEC_I_CONTINUE_NEEDED) {
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer > 0 &&
            ib[1].cbBuffer <= incoming.size()) {
          size_t extra = ib[1].cbBuffer;
          std::vector<char> keep(incoming.end() - (ptrdiff_t)extra, incoming.end());
          incoming.swap(keep);
        } else {
          incoming.clear();
        }
      }
      char tmp[8192];
      int n = recv(t.sock, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        err = L"TLS handshake failed while reading";
        return false;
      }
      incoming.insert(incoming.end(), tmp, tmp + n);
      continue;
    }

    err = L"TLS handshake failed (" + std::to_wstring((long)ss) + L")";
    return false;
  }

  ss = QueryContextAttributes(&t.ctx, SECPKG_ATTR_STREAM_SIZES, &t.sizes);
  if (ss != SEC_E_OK || t.sizes.cbMaximumMessage == 0) {
    err = L"TLS stream setup failed";
    return false;
  }
  return true;
}

static bool TlsSend(TlsConn& t, const char* data, size_t len, std::wstring& err) {
  size_t off = 0;
  while (off < len) {
    size_t chunk = std::min<size_t>(len - off, t.sizes.cbMaximumMessage);
    std::vector<char> buf(t.sizes.cbHeader + chunk + t.sizes.cbTrailer);
    memcpy(buf.data() + t.sizes.cbHeader, data + off, chunk);

    SecBuffer sb[4]{};
    sb[0].BufferType = SECBUFFER_STREAM_HEADER;
    sb[0].pvBuffer = buf.data();
    sb[0].cbBuffer = t.sizes.cbHeader;
    sb[1].BufferType = SECBUFFER_DATA;
    sb[1].pvBuffer = buf.data() + t.sizes.cbHeader;
    sb[1].cbBuffer = (ULONG)chunk;
    sb[2].BufferType = SECBUFFER_STREAM_TRAILER;
    sb[2].pvBuffer = buf.data() + t.sizes.cbHeader + chunk;
    sb[2].cbBuffer = t.sizes.cbTrailer;
    sb[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{ SECBUFFER_VERSION, 4, sb };

    SECURITY_STATUS ss = EncryptMessage(&t.ctx, 0, &desc, 0);
    if (ss != SEC_E_OK) {
      err = L"TLS encrypt failed";
      return false;
    }
    int total = (int)(sb[0].cbBuffer + sb[1].cbBuffer + sb[2].cbBuffer);
    if (!SendAllSock(t.sock, buf.data(), total)) {
      err = L"TLS send failed";
      return false;
    }
    off += chunk;
  }
  return true;
}

// Append decrypted bytes to t.plain. false means the peer closed or an error.
static bool TlsRecv(TlsConn& t, std::wstring& err) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (;;) {
    if (std::chrono::steady_clock::now() > deadline) {
      err = L"TLS read timed out";
      return false;
    }
    if (t.enc.empty()) {
      char tmp[16384];
      int n = recv(t.sock, tmp, sizeof(tmp), 0);
      if (n <= 0) return false;
      t.enc.insert(t.enc.end(), tmp, tmp + n);
    }

    SecBuffer sb[4]{};
    sb[0].BufferType = SECBUFFER_DATA;
    sb[0].pvBuffer = t.enc.data();
    sb[0].cbBuffer = (ULONG)t.enc.size();
    sb[1].BufferType = SECBUFFER_EMPTY;
    sb[2].BufferType = SECBUFFER_EMPTY;
    sb[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{ SECBUFFER_VERSION, 4, sb };

    SECURITY_STATUS ss = DecryptMessage(&t.ctx, &desc, 0, nullptr);
    if (ss == SEC_E_INCOMPLETE_MESSAGE) {
      char tmp[16384];
      int n = recv(t.sock, tmp, sizeof(tmp), 0);
      if (n <= 0) return false;
      t.enc.insert(t.enc.end(), tmp, tmp + n);
      continue;
    }
    if (ss == SEC_I_CONTEXT_EXPIRED || ss == SEC_I_RENEGOTIATE) return false;
    if (ss != SEC_E_OK) {
      err = L"TLS decrypt failed";
      return false;
    }

    char* data = nullptr;
    ULONG dataLen = 0;
    char* extra = nullptr;
    ULONG extraLen = 0;
    for (int i = 0; i < 4; ++i) {
      if (sb[i].BufferType == SECBUFFER_DATA) {
        data = (char*)sb[i].pvBuffer;
        dataLen = sb[i].cbBuffer;
      } else if (sb[i].BufferType == SECBUFFER_EXTRA) {
        extra = (char*)sb[i].pvBuffer;
        extraLen = sb[i].cbBuffer;
      }
    }
    if (data && dataLen) t.plain.insert(t.plain.end(), data, data + dataLen);
    if (extra && extraLen) {
      std::vector<char> rest(extra, extra + extraLen);
      t.enc.swap(rest);
    } else {
      t.enc.clear();
    }
    if (!t.plain.empty()) return true;
  }
}

static bool ReadHttpHeaders(TlsConn& t, std::string& header, std::string& preBody, std::wstring& err) {
  std::string acc;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (acc.find("\r\n\r\n") == std::string::npos) {
    if (std::chrono::steady_clock::now() > deadline) {
      err = L"Timed out waiting for download headers";
      return false;
    }
    if (t.plain.empty() && !TlsRecv(t, err) && t.plain.empty()) {
      if (err.empty()) err = L"Connection closed before download headers";
      return false;
    }
    acc.append(t.plain.data(), t.plain.size());
    t.plain.clear();
    if (acc.size() > 256 * 1024) {
      err = L"Download headers too large";
      return false;
    }
  }
  size_t sep = acc.find("\r\n\r\n");
  header = acc.substr(0, sep);
  preBody = acc.substr(sep + 4);
  return true;
}

static int HttpStatusCode(const std::string& header) {
  size_t p = header.find("HTTP/");
  if (p == std::string::npos) return 0;
  size_t sp = header.find(' ', p);
  if (sp == std::string::npos || sp + 4 > header.size()) return 0;
  return atoi(header.c_str() + sp + 1);
}

static std::string HeaderValue(const std::string& header, const char* name) {
  std::string lower = header;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  std::string key = name;
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  key += ':';
  size_t p = 0;
  while (p < lower.size()) {
    size_t lineEnd = lower.find("\r\n", p);
    if (lineEnd == std::string::npos) lineEnd = lower.size();
    if (lower.compare(p, key.size(), key) == 0) {
      size_t vs = p + key.size();
      while (vs < lineEnd && (header[vs] == ' ' || header[vs] == '\t')) vs++;
      return header.substr(vs, lineEnd - vs);
    }
    if (lineEnd == lower.size()) break;
    p = lineEnd + 2;
  }
  return {};
}

static SOCKET OpenProxyTunnel(const ProxyConfig& cfg, const std::string& host, int port, std::wstring& err) {
  SOCKET s = INVALID_SOCKET;
  if (!ConnectTcp(cfg.hostA(), cfg.port, s, err)) return INVALID_SOCKET;
  int to = 20000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
  bool ok = (cfg.type == ProxyType::SOCKS5)
                ? Socks5Connect(s, cfg, host, port, err)
                : HttpProxyConnect(s, cfg, host, port, err);
  if (!ok) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

static bool DownloadSpeedFile(const ProxyConfig& cfg, size_t& outBytes, int& outMs, std::wstring& err) {
  outBytes = 0;
  outMs = 0;
  HttpsUrl cur;
  if (!ParseHttpsUrl(kSpeedTestUrl, cur)) {
    err = L"Bad speed-test URL";
    return false;
  }

  for (int hop = 0; hop < 5; ++hop) {
    SOCKET s = OpenProxyTunnel(cfg, cur.host, cur.port, err);
    if (s == INVALID_SOCKET) {
      if (err.empty()) err = L"Could not tunnel to the download server";
      return false;
    }

    bool redirect = false;
    HttpsUrl next;
    {
      TlsConn tls;
      tls.sock = s;
      if (!TlsHandshake(tls, cur.host, err)) {
        closesocket(s);
        return false;
      }

      std::string req = "GET " + cur.path + " HTTP/1.1\r\n";
      req += "Host: " + cur.host + "\r\n";
      req += "User-Agent: ProxyPiTester/1.3\r\n";
      req += "Accept: */*\r\n";
      req += "Connection: close\r\n\r\n";
      if (!TlsSend(tls, req.data(), req.size(), err)) {
        closesocket(s);
        return false;
      }

      std::string header, preBody;
      if (!ReadHttpHeaders(tls, header, preBody, err)) {
        closesocket(s);
        return false;
      }

      int status = HttpStatusCode(header);
      if (status >= 300 && status < 400) {
        std::string loc = HeaderValue(header, "location");
        if (loc.rfind("https://", 0) == 0) {
          if (!ParseHttpsUrl(loc, next)) {
            err = L"Download redirect was not a usable HTTPS URL";
            closesocket(s);
            return false;
          }
        } else if (!loc.empty() && loc[0] == '/') {
          next = cur;
          next.path = loc;
        } else {
          err = L"Download redirect was not HTTPS";
          closesocket(s);
          return false;
        }
        redirect = true;
      } else if (status != 200) {
        err = L"Download HTTP " + std::to_wstring(status);
        closesocket(s);
        return false;
      } else {
        std::string pending = preBody;
        std::string lenStr = HeaderValue(header, "content-length");
        bool haveLen = !lenStr.empty();
        size_t contentLen = haveLen ? (size_t)strtoull(lenStr.c_str(), nullptr, 10) : 0;
        auto t0 = std::chrono::steady_clock::now();
        const auto deadline = t0 + std::chrono::milliseconds(kSpeedSampleMs);

        auto take = [&](const char* p, size_t n) {
          if (n == 0 || outBytes >= kSpeedMaxBytes) return;
          size_t room = kSpeedMaxBytes - outBytes;
          if (n > room) n = room;
          if (haveLen && outBytes + n > contentLen) {
            if (contentLen > outBytes) n = contentLen - outBytes;
            else n = 0;
          }
          outBytes += n;
          (void)p;
        };

        take(pending.data(), pending.size());
        while (outBytes < kSpeedMaxBytes &&
               !(haveLen && outBytes >= contentLen) &&
               std::chrono::steady_clock::now() < deadline) {
          if (tls.plain.empty() && !TlsRecv(tls, err)) break;
          if (tls.plain.empty()) break;
          take(tls.plain.data(), tls.plain.size());
          tls.plain.clear();
        }
        auto t1 = std::chrono::steady_clock::now();
        outMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (outMs < 1) outMs = 1;
        closesocket(s);
        if (outBytes < kSpeedMinBytes) {
          err = L"Downloaded only " + std::to_wstring((long long)outBytes) +
                L" bytes from 100mb.bin (need a larger sample)";
          return false;
        }
        err.clear();
        return true;
      }
    }
    closesocket(s);
    if (!redirect) {
      err = L"Download failed";
      return false;
    }
    cur = next;
  }

  err = L"Too many redirects fetching 100mb.bin";
  return false;
}

SpeedTestResult SpeedTestProxy(const ProxyConfig& cfg) {
  SpeedTestResult res;
  std::wstring err;
  if (!InitWinsock(err)) {
    res.message = err;
    return res;
  }

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

  size_t bytes = 0;
  int downloadMs = 0;
  bool ok = DownloadSpeedFile(cfg, bytes, downloadMs, err);
  if (!ok) {
    res.message = L"Exit IP " + res.detectedIp + L". " + err;
    return res;
  }

  res.bytesDownloaded = bytes;
  res.downloadMs = downloadMs;
  // Mbps = (bytes * 8) / (ms / 1000) / 1_000_000 = bytes * 8 / ms / 1000
  res.downloadMbps = (double)bytes * 8.0 / (double)res.downloadMs / 1000.0;
  res.success = true;

  wchar_t speedBuf[64];
  swprintf_s(speedBuf, L"%.2f", res.downloadMbps);
  double mb = (double)bytes / (1024.0 * 1024.0);
  wchar_t mbBuf[32];
  swprintf_s(mbBuf, L"%.1f", mb);
  res.message = L"Speed test complete - " + std::wstring(speedBuf) +
                L" Mbps (" + std::wstring(mbBuf) + L" MB of 100mb.bin)";
  return res;
}

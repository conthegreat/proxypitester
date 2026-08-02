#pragma once

#include "common.h"
#include <string>

struct ProxyTestResult {
  bool success = false;
  std::wstring message;
  std::wstring detectedIp;
  int latencyMs = 0;
};

// Initialize Winsock (call once)
bool InitWinsock(std::wstring& err);

// Connect raw TCP to a host:port
bool ConnectTcp(const std::string& host, int port, SOCKET& outSock, std::wstring& err);

// Perform full SOCKS5 handshake + CONNECT for target (leaves socket ready)
bool Socks5Connect(SOCKET s, const ProxyConfig& cfg,
                   const std::string& targetHost, int targetPort,
                   std::wstring& err);

// Perform HTTP proxy CONNECT or direct for target (leaves socket ready for HTTP traffic)
bool HttpProxyConnect(SOCKET s, const ProxyConfig& cfg,
                      const std::string& targetHost, int targetPort,
                      std::wstring& err);

// High level test function
ProxyTestResult TestProxy(const ProxyConfig& cfg, const std::string& testHost = "api.ipify.org", int testPort = 80);

// Send a simple HTTP request over an already-tunneled socket and return body
bool SendHttpGet(SOCKET s, const std::string& host, const std::string& path,
                 std::string& outBody, std::wstring& err);

// Speed test: download through the proxy and measure throughput
struct SpeedTestResult {
  bool success = false;
  std::wstring message;
  std::wstring detectedIp;
  int latencyMs = 0;          // connect + handshake + first IP check
  double downloadMbps = 0.0;  // measured download speed
  size_t bytesDownloaded = 0;
  int downloadMs = 0;
};

// Downloads a ~1–5 MB payload through the proxy (plain HTTP) to measure Mbps.
// Optionally reuses a recent IP check (pass empty to also fetch IP).
SpeedTestResult SpeedTestProxy(const ProxyConfig& cfg);
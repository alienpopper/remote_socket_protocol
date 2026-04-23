// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_http_helper.h"

#include <array>
#include <limits>
#include <string>

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "url/gurl.h"

namespace rsp {

HttpResult::HttpResult() = default;
HttpResult::HttpResult(const HttpResult&) = default;
HttpResult::HttpResult(HttpResult&&) = default;
HttpResult& HttpResult::operator=(const HttpResult&) = default;
HttpResult& HttpResult::operator=(HttpResult&&) = default;
HttpResult::~HttpResult() = default;

namespace {

using SocketIoResult =
#if BUILDFLAG(IS_WIN)
    int;
#else
    ssize_t;
#endif

SocketIoResult SocketRead(intptr_t socket, char* buffer, size_t length) {
#if BUILDFLAG(IS_WIN)
  const size_t bounded_length =
      length > static_cast<size_t>(std::numeric_limits<int>::max())
          ? static_cast<size_t>(std::numeric_limits<int>::max())
          : length;
  return ::recv(static_cast<SOCKET>(socket), buffer,
                static_cast<int>(bounded_length), 0);
#else
  return ::read(static_cast<int>(socket), buffer, length);
#endif
}

SocketIoResult SocketWrite(intptr_t socket,
                           const uint8_t* buffer,
                           size_t length) {
#if BUILDFLAG(IS_WIN)
  const size_t bounded_length =
      length > static_cast<size_t>(std::numeric_limits<int>::max())
          ? static_cast<size_t>(std::numeric_limits<int>::max())
          : length;
  return ::send(static_cast<SOCKET>(socket),
                reinterpret_cast<const char*>(buffer),
                static_cast<int>(bounded_length), 0);
#else
  return ::write(static_cast<int>(socket), buffer, length);
#endif
}

void CloseSocket(intptr_t socket) {
#if BUILDFLAG(IS_WIN)
  ::closesocket(static_cast<SOCKET>(socket));
#else
  ::close(static_cast<int>(socket));
#endif
}

std::string BuildHttpRequest(const network::ResourceRequest& request) {
  const GURL& url = request.url;
  std::string out =
      base::StringPrintf("%s %s HTTP/1.1\r\n", request.method.c_str(),
                         url.PathForRequest().c_str());
  out += "Host: " + std::string(url.host()) + "\r\n";
  out += "Connection: close\r\n";

  net::HttpRequestHeaders::Iterator it(request.headers);
  while (it.GetNext()) {
    if (base::EqualsCaseInsensitiveASCII(it.name(), "host") ||
        base::EqualsCaseInsensitiveASCII(it.name(), "connection")) {
      continue;
    }
    out += it.name() + ": " + it.value() + "\r\n";
  }
  out += "\r\n";
  return out;
}

std::string ReadUntil(intptr_t socket, const std::string& delimiter) {
  std::string buf;
  std::array<char, 4096> tmp;
  while (buf.find(delimiter) == std::string::npos) {
    const SocketIoResult n = SocketRead(socket, tmp.data(), tmp.size());
    if (n <= 0) {
      return std::string();
    }
    buf.append(tmp.data(), static_cast<size_t>(n));
  }
  return buf.substr(0, buf.find(delimiter) + delimiter.size());
}

void ReadAll(intptr_t socket, std::string& out) {
  std::array<char, 4096> tmp;
  while (true) {
    const SocketIoResult n = SocketRead(socket, tmp.data(), tmp.size());
    if (n <= 0) {
      break;
    }
    out.append(tmp.data(), static_cast<size_t>(n));
  }
}

}  // namespace

HttpResult DoHttpRequest(intptr_t socket,
                         const network::ResourceRequest& request) {
  HttpResult result;
  const std::string req_str = BuildHttpRequest(request);

  // Use span::subspan to avoid raw pointer arithmetic (-Wunsafe-buffer-usage).
  auto remaining = base::as_bytes(base::span(req_str));
  while (!remaining.empty()) {
    const SocketIoResult n =
        SocketWrite(socket, remaining.data(), remaining.size());
    if (n <= 0) {
      result.net_error = net::ERR_CONNECTION_RESET;
      CloseSocket(socket);
      return result;
    }
    remaining = remaining.subspan(static_cast<size_t>(n));
  }

  const std::string header_text = ReadUntil(socket, "\r\n\r\n");
  if (header_text.empty()) {
    result.net_error = net::ERR_EMPTY_RESPONSE;
    CloseSocket(socket);
    return result;
  }

  const std::string raw = net::HttpUtil::AssembleRawHeaders(header_text);
  result.headers = base::MakeRefCounted<net::HttpResponseHeaders>(raw);
  ReadAll(socket, result.body);
  CloseSocket(socket);
  result.net_error = net::OK;
  return result;
}

}  // namespace rsp

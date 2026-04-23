// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_http_helper.h"

#include <array>
#include <string>
#include <unistd.h>

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

std::string ReadUntil(int fd, const std::string& delimiter) {
  std::string buf;
  std::array<char, 4096> tmp;
  while (buf.find(delimiter) == std::string::npos) {
    ssize_t n = ::read(fd, tmp.data(), tmp.size());
    if (n <= 0) {
      return std::string();
    }
    buf.append(tmp.data(), static_cast<size_t>(n));
  }
  return buf.substr(0, buf.find(delimiter) + delimiter.size());
}

void ReadAll(int fd, std::string& out) {
  std::array<char, 4096> tmp;
  while (true) {
    ssize_t n = ::read(fd, tmp.data(), tmp.size());
    if (n <= 0) {
      break;
    }
    out.append(tmp.data(), static_cast<size_t>(n));
  }
}

}  // namespace

HttpResult DoHttpRequest(int fd, const network::ResourceRequest& request) {
  HttpResult result;
  const std::string req_str = BuildHttpRequest(request);

  // Use span::subspan to avoid raw pointer arithmetic (-Wunsafe-buffer-usage).
  auto remaining = base::as_bytes(base::span(req_str));
  while (!remaining.empty()) {
    ssize_t n =
        ::write(fd, remaining.data(), static_cast<size_t>(remaining.size()));
    if (n <= 0) {
      result.net_error = net::ERR_CONNECTION_RESET;
      ::close(fd);
      return result;
    }
    remaining = remaining.subspan(static_cast<size_t>(n));
  }

  const std::string header_text = ReadUntil(fd, "\r\n\r\n");
  if (header_text.empty()) {
    result.net_error = net::ERR_EMPTY_RESPONSE;
    ::close(fd);
    return result;
  }

  const std::string raw = net::HttpUtil::AssembleRawHeaders(header_text);
  result.headers = base::MakeRefCounted<net::HttpResponseHeaders>(raw);
  ReadAll(fd, result.body);
  ::close(fd);
  result.net_error = net::OK;
  return result;
}

}  // namespace rsp

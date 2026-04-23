// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_HTTP_HELPER_H_
#define CHROME_BROWSER_RSP_RSP_HTTP_HELPER_H_

#include <string>

#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"

namespace rsp {

struct HttpResult {
  HttpResult();
  HttpResult(const HttpResult&);
  HttpResult(HttpResult&&);
  HttpResult& operator=(const HttpResult&);
  HttpResult& operator=(HttpResult&&);
  ~HttpResult();

  int net_error = net::ERR_FAILED;
  scoped_refptr<net::HttpResponseHeaders> headers;
  std::string body;
};

// Sends |request| as a plain HTTP/1.1 request over the raw socket |fd|,
// reads the response, closes |fd|, and returns the result.
// Must be called on a thread that allows blocking I/O.
HttpResult DoHttpRequest(int fd, const network::ResourceRequest& request);

}  // namespace rsp

#endif  // CHROME_BROWSER_RSP_RSP_HTTP_HELPER_H_

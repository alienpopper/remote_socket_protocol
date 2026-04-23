// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_HTTP_HELPER_H_
#define CHROME_BROWSER_RSP_RSP_HTTP_HELPER_H_

#include <stdint.h>
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

// Sends |request| as a plain HTTP/1.1 request over |socket|,
// reads the response, closes the socket, and returns the result.
// Must be called on a thread that allows blocking I/O.
HttpResult DoHttpRequest(intptr_t socket,
                         const network::ResourceRequest& request);

}  // namespace rsp

#endif  // CHROME_BROWSER_RSP_RSP_HTTP_HELPER_H_

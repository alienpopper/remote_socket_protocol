// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_URL_LOADER_FACTORY_H_
#define CHROME_BROWSER_RSP_RSP_URL_LOADER_FACTORY_H_

namespace network {
class URLLoaderFactoryBuilder;
}

class Profile;

namespace rsp {

// Static helpers for hooking RSP tab profiles into the URL loader pipeline.
// Phase 2 stub: currently a no-op that logs and returns.  A future phase will
// wrap the factory to intercept http/https traffic and route it via the RSP
// network stack.
class RspURLLoaderFactory {
 public:
  // Called from ChromeContentBrowserClient::WillCreateURLLoaderFactory().
  // If |profile| is an RSP tab profile, wraps |factory_builder| with an RSP
  // interceptor.  Currently a stub (no-op).
  static void MaybeIntercept(Profile* profile,
                             network::URLLoaderFactoryBuilder& factory_builder);

  RspURLLoaderFactory() = delete;
};

}  // namespace rsp

#endif  // CHROME_BROWSER_RSP_RSP_URL_LOADER_FACTORY_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_
#define CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_

#include <map>
#include <memory>
#include <string>

#include "base/no_destructor.h"
#include "chrome/browser/rsp/rsp_config.h"

// Singleton that manages shared RSP connections keyed by (rm_addr, rs_node_id).
// Multiple RSP tabs using the same RM+RS share one connection.
// TODO(rsp): Wire up actual RSP client when network layer is implemented.
class RspConnectionManager {
 public:
  static RspConnectionManager* GetInstance();

  // Returns an opaque connection handle for the given config.
  // Creates a new connection if one doesn't exist; increments ref count.
  std::string GetOrCreate(const RspTabConfig& config);

  // Decrements ref count for the connection. Disconnects when ref=0.
  void Release(const std::string& connection_key);

  RspConnectionManager(const RspConnectionManager&) = delete;
  RspConnectionManager& operator=(const RspConnectionManager&) = delete;

 private:
  friend class base::NoDestructor<RspConnectionManager>;
  RspConnectionManager();
  ~RspConnectionManager();

  struct ConnectionEntry {
    int ref_count = 0;
    // TODO(rsp): std::shared_ptr<RSPClient> client;
  };

  std::map<std::string, ConnectionEntry> connections_;
};

#endif  // CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_

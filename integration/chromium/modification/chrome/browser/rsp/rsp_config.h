// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_CONFIG_H_
#define CHROME_BROWSER_RSP_RSP_CONFIG_H_

#include <string>

// Configuration for an RSP tab connection.
struct RspTabConfig {
  // Address of the Resource Manager (e.g. "localhost:8080").
  std::string rm_addr;

  // Node ID of the bsd_sockets Resource Service to route traffic through.
  std::string rs_node_id;

  // Optional: node ID of a Name Service for resolving rsp:// names.
  std::string ns_node_id;
};

#endif  // CHROME_BROWSER_RSP_RSP_CONFIG_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_
#define CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/no_destructor.h"
#include "chrome/browser/rsp/rsp_config.h"

class Profile;

// Singleton that manages shared RSP connections keyed by (rm_addr, rs_node_id).
// Multiple RSP tabs using the same RM+RS share one connection (ref-counted).
//
// IMPORTANT: This class is intentionally a complete facade — callers MUST NOT
// include any RSP headers.  All RSP types are hidden behind the Impl pimpl.
class RspConnectionManager {
 public:
  static RspConnectionManager* GetInstance();

  // Creates (or increments ref-count of) an RSP client connection for the
  // given config.  Returns the connection key, or empty string on failure.
  std::string GetOrCreate(const RspTabConfig& config);

  // Associates |profile| with |connection_key| so that GetKeyForProfile()
  // can find it later.  Called by NewRspTab() right after GetOrCreate().
  void RegisterProfile(Profile* profile, const std::string& connection_key);

  // Returns the connection key for the given RSP OTR profile, or empty string
  // if not registered.
  std::string GetKeyForProfile(Profile* profile) const;

  // Decrements the ref count. Stops and destroys the RSP client when it
  // reaches zero.
  void Release(const std::string& connection_key);

  // Unregisters a profile mapping (called when the OTR profile is destroyed).
  void UnregisterProfile(Profile* profile);

  // Opens a TCP connection to |host_port| (e.g. "example.com:80") through the
  // bsd_sockets Resource Service identified by |connection_key|.
  // Returns the raw socket handle on success, or -1 on failure.
  // The caller owns the socket and must close/closesocket it when done.
  intptr_t ConnectTCPSocket(const std::string& connection_key,
                            const std::string& host_port);

  // Opens a plain HTTP byte stream to an httpd Resource Service node. The
  // target node is supplied per rsp:// URL authority. TLS is not used.
  // Returns the raw socket handle on success, or -1 on failure.
  // The caller owns the socket and must close/closesocket it when done.
  intptr_t ConnectHttpSocket(const std::string& connection_key,
                             const std::string& httpd_node_id,
                             const std::string& virtual_host);

  RspConnectionManager(const RspConnectionManager&) = delete;
  RspConnectionManager& operator=(const RspConnectionManager&) = delete;

 private:
  friend class base::NoDestructor<RspConnectionManager>;
  RspConnectionManager();
  ~RspConnectionManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_

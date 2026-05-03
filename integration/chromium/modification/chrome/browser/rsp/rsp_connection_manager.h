// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_
#define CHROME_BROWSER_RSP_RSP_CONNECTION_MANAGER_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/no_destructor.h"
#include "chrome/browser/rsp/rsp_config.h"

class Profile;

struct RspConnectionHealth {
  bool rm_reachable = false;
  bool rs_configured = false;
  bool rs_reachable = false;
  std::string message;
};

// Singleton that manages per-RSP-tab proxy configuration keyed by profile.
// This is intentionally proxy-only: it configures Chromium proxy prefs for the
// tab profile but does not install URL loaders or open RSP sockets.
class RspConnectionManager {
 public:
  static RspConnectionManager* GetInstance();

  // Creates (or increments ref-count of) an RSP client connection for the
  // given config.  Returns the connection key, or empty string on failure.
  std::string GetOrCreate(const RspTabConfig& config);

  // Associates |profile| with |connection_key| so that GetKeyForProfile()
  // can find it later.  Called by NewRspTab() right after GetOrCreate().
  void RegisterProfile(Profile* profile,
                       const std::string& connection_key,
                       const RspTabConfig& config);

  // Returns the connection key for the given RSP OTR profile, or empty string
  // if not registered.
  std::string GetKeyForProfile(Profile* profile) const;

  // Returns the current tab-scoped RSP config for |profile|.
  RspTabConfig GetConfigForProfile(Profile* profile) const;

  // Switches one RSP tab profile to a different RM / bsd_sockets service.
  // This updates only |profile|; other RSP tabs keep their existing proxy.
  bool SetConfigForProfile(Profile* profile, const RspTabConfig& config);

  // Returns cached connection state plus an active ping to the selected
  // bsd_sockets node when one is configured.
  RspConnectionHealth GetHealthForProfile(Profile* profile);

  // Decrements the ref count. Stops and destroys the RSP client when it
  // reaches zero.
  void Release(const std::string& connection_key);

  // Unregisters a profile mapping (called when the OTR profile is destroyed).
  void UnregisterProfile(Profile* profile);

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

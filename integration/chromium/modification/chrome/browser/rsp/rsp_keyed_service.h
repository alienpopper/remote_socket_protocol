// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_KEYED_SERVICE_H_
#define CHROME_BROWSER_RSP_RSP_KEYED_SERVICE_H_

#include "chrome/browser/rsp/rsp_config.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

// Keyed service that stores the RSP configuration for an RSP OTR profile.
// Lifetime is tied to the OTR profile.
class RspKeyedService : public KeyedService {
 public:
  explicit RspKeyedService(const RspTabConfig& config);
  ~RspKeyedService() override;

  const RspTabConfig& config() const { return config_; }

 private:
  RspTabConfig config_;
};

#endif  // CHROME_BROWSER_RSP_RSP_KEYED_SERVICE_H_

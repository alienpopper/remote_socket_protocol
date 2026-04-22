// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_url_loader_factory.h"

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"

namespace rsp {

// static
void RspURLLoaderFactory::MaybeIntercept(
    Profile* profile,
    network::URLLoaderFactoryBuilder& factory_builder) {
  if (!profile || !profile->IsOffTheRecord()) {
    return;
  }
  if (!profile->GetOTRProfileID().IsRspTab()) {
    return;
  }
  // TODO(rsp): Wrap factory_builder with an RSP interceptor that routes
  // http/https requests through the RSP network stack (Phase 2+).
  DVLOG(1) << "RspURLLoaderFactory::MaybeIntercept: RSP tab profile detected"
           << " (stub - no-op for now)";
}

}  // namespace rsp

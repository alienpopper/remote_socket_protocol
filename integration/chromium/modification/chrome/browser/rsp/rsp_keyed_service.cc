// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_keyed_service.h"

RspKeyedService::RspKeyedService(const RspTabConfig& config)
    : config_(config) {}

RspKeyedService::~RspKeyedService() = default;

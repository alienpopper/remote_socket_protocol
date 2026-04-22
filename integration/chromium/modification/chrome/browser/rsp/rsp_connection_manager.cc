// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_connection_manager.h"

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"

// static
RspConnectionManager* RspConnectionManager::GetInstance() {
  static base::NoDestructor<RspConnectionManager> instance;
  return instance.get();
}

RspConnectionManager::RspConnectionManager() = default;
RspConnectionManager::~RspConnectionManager() = default;

std::string RspConnectionManager::GetOrCreate(const RspTabConfig& config) {
  std::string key = base::StringPrintf("%s|%s", config.rm_addr.c_str(),
                                       config.rs_node_id.c_str());
  auto& entry = connections_[key];
  entry.ref_count++;
  LOG(INFO) << "RspConnectionManager: GetOrCreate key=" << key
            << " ref=" << entry.ref_count;
  // TODO(rsp): Create actual RSP client connection here.
  return key;
}

void RspConnectionManager::Release(const std::string& connection_key) {
  auto it = connections_.find(connection_key);
  if (it == connections_.end()) {
    return;
  }
  it->second.ref_count--;
  LOG(INFO) << "RspConnectionManager: Release key=" << connection_key
            << " ref=" << it->second.ref_count;
  if (it->second.ref_count <= 0) {
    // TODO(rsp): Disconnect RSP client.
    connections_.erase(it);
  }
}

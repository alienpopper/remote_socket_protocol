// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_connection_manager.h"

#include <map>
#include <mutex>
#include <string>

// RSP C API — pure C header, no protobuf.
#include "client/cpp/rsp_c_api.h"

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"

struct RspConnectionManager::Impl {
  struct ConnectionEntry {
    int ref_count = 0;
    RspBridgeHandle bridge = nullptr;
  };

  std::map<std::string, ConnectionEntry> connections;  // key -> entry
  std::map<Profile*, std::string> profile_keys;        // profile -> key
  std::mutex mutex;
};

// static
RspConnectionManager* RspConnectionManager::GetInstance() {
  static base::NoDestructor<RspConnectionManager> instance;
  return instance.get();
}

RspConnectionManager::RspConnectionManager()
    : impl_(std::make_unique<Impl>()) {}

RspConnectionManager::~RspConnectionManager() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (auto& [key, entry] : impl_->connections) {
    if (entry.bridge) {
      rsp_bridge_destroy(entry.bridge);
    }
  }
}

std::string RspConnectionManager::GetOrCreate(const RspTabConfig& config) {
  const std::string key = base::StringPrintf(
      "%s|%s", config.rm_addr.c_str(), config.rs_node_id.c_str());

  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto& entry = impl_->connections[key];
  if (entry.ref_count == 0) {
    RspBridgeHandle bridge =
        rsp_bridge_create(config.rm_addr.c_str(), config.rs_node_id.c_str());
    if (!bridge) {
      LOG(ERROR) << "RspConnectionManager: failed to connect to RM at "
                 << config.rm_addr;
      impl_->connections.erase(key);
      return std::string();
    }

    LOG(INFO) << "RspConnectionManager: connected to RM at " << config.rm_addr
              << " rs_node_id=" << config.rs_node_id;
    entry.bridge = bridge;
  }

  entry.ref_count++;
  LOG(INFO) << "RspConnectionManager: GetOrCreate key=" << key
            << " ref=" << entry.ref_count;
  return key;
}

void RspConnectionManager::RegisterProfile(Profile* profile,
                                           const std::string& connection_key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->profile_keys[profile] = connection_key;
}

std::string RspConnectionManager::GetKeyForProfile(Profile* profile) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->profile_keys.find(profile);
  return it != impl_->profile_keys.end() ? it->second : std::string();
}

void RspConnectionManager::Release(const std::string& connection_key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->connections.find(connection_key);
  if (it == impl_->connections.end()) {
    return;
  }

  it->second.ref_count--;
  LOG(INFO) << "RspConnectionManager: Release key=" << connection_key
            << " ref=" << it->second.ref_count;

  if (it->second.ref_count <= 0) {
    rsp_bridge_destroy(it->second.bridge);
    impl_->connections.erase(it);
  }
}

void RspConnectionManager::UnregisterProfile(Profile* profile) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->profile_keys.erase(profile);
}

intptr_t RspConnectionManager::ConnectTCPSocket(
    const std::string& connection_key,
    const std::string& host_port) {
  RspBridgeHandle bridge = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->connections.find(connection_key);
    if (it == impl_->connections.end()) {
      LOG(ERROR) << "RspConnectionManager::ConnectTCPSocket: key not found: "
                 << connection_key;
      return -1;
    }
    bridge = it->second.bridge;
  }

  if (!bridge) {
    return -1;
  }

  const intptr_t socket = rsp_bridge_connect_tcp(bridge, host_port.c_str());
  if (socket < 0) {
    LOG(WARNING) << "RspConnectionManager::ConnectTCPSocket: failed for "
                 << host_port;
  }
  return socket;
}

intptr_t RspConnectionManager::ConnectHttpSocket(
    const std::string& connection_key,
    const std::string& httpd_node_id,
    const std::string& virtual_host) {
  RspBridgeHandle bridge = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->connections.find(connection_key);
    if (it == impl_->connections.end()) {
      LOG(ERROR) << "RspConnectionManager::ConnectHttpSocket: key not found: "
                 << connection_key;
      return -1;
    }
    bridge = it->second.bridge;
  }

  if (!bridge) {
    return -1;
  }

  const intptr_t socket = rsp_bridge_connect_http(
      bridge, httpd_node_id.c_str(), virtual_host.c_str());
  if (socket < 0) {
    LOG(WARNING) << "RspConnectionManager::ConnectHttpSocket: failed for "
                 << httpd_node_id;
  }
  return socket;
}

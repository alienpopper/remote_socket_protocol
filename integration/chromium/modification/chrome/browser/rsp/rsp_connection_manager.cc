// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_connection_manager.h"

#include <cctype>
#include <map>
#include <mutex>
#include <string>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"

namespace {

std::string MakeConnectionKey(const RspTabConfig& config) {
  return base::StringPrintf("%s|%s", config.rm_addr.c_str(),
                            config.rs_node_id.c_str());
}

std::string MakeProxyHostLabel(const std::string& value) {
  std::string label;
  for (char ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      label.push_back(static_cast<char>(std::tolower(uch)));
    } else if (ch == '-' && !label.empty() && label.back() != '-') {
      label.push_back(ch);
    }
  }

  while (!label.empty() && label.back() == '-') {
    label.pop_back();
  }
  if (label.empty()) {
    label = "unconfigured";
  }
  if (label.size() > 63) {
    label.resize(63);
    while (!label.empty() && label.back() == '-') {
      label.pop_back();
    }
  }
  return label.empty() ? "unconfigured" : label;
}

std::string MakeRspProxyServer(const RspTabConfig& config) {
  if (config.rs_node_id.empty()) {
    return std::string();
  }

  return base::StringPrintf("socks5://%s.rsp-proxy.invalid:1080",
                            MakeProxyHostLabel(config.rs_node_id).c_str());
}

void ApplyProxyConfigForProfile(Profile* profile, const RspTabConfig& config) {
  if (!profile) {
    return;
  }

  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return;
  }

  const std::string proxy_server = MakeRspProxyServer(config);
  if (proxy_server.empty()) {
    prefs->SetDict(proxy_config::prefs::kProxy,
                   ProxyConfigDictionary::CreateDirect());
    LOG(INFO) << "RspConnectionManager: configured direct network for RSP "
                 "profile with no bsd_sockets node";
    return;
  }

  prefs->SetDict(proxy_config::prefs::kProxy,
                 ProxyConfigDictionary::CreateFixedServers(
                     proxy_server, /*bypass_list=*/std::string()));
  LOG(INFO) << "RspConnectionManager: configured per-tab proxy "
            << proxy_server << " for RM " << config.rm_addr;
}

}  // namespace

struct RspConnectionManager::Impl {
  struct ConnectionEntry {
    int ref_count = 0;
  };

  std::map<std::string, ConnectionEntry> connections;  // key -> entry
  std::map<Profile*, std::string> profile_keys;        // profile -> key
  std::map<Profile*, RspTabConfig> profile_configs;     // profile -> config
  std::mutex mutex;
};

// static
RspConnectionManager* RspConnectionManager::GetInstance() {
  static base::NoDestructor<RspConnectionManager> instance;
  return instance.get();
}

RspConnectionManager::RspConnectionManager()
    : impl_(std::make_unique<Impl>()) {}

RspConnectionManager::~RspConnectionManager() = default;

std::string RspConnectionManager::GetOrCreate(const RspTabConfig& config) {
  const std::string key = MakeConnectionKey(config);

  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto& entry = impl_->connections[key];
  if (entry.ref_count == 0) {
    LOG(INFO) << "RspConnectionManager: registered proxy-only config for RM "
              << config.rm_addr << " rs_node_id=" << config.rs_node_id;
  }

  entry.ref_count++;
  LOG(INFO) << "RspConnectionManager: GetOrCreate key=" << key
            << " ref=" << entry.ref_count;
  return key;
}

void RspConnectionManager::RegisterProfile(Profile* profile,
                                           const std::string& connection_key,
                                           const RspTabConfig& config) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->profile_keys[profile] = connection_key;
    impl_->profile_configs[profile] = config;
  }
  ApplyProxyConfigForProfile(profile, config);
}

std::string RspConnectionManager::GetKeyForProfile(Profile* profile) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->profile_keys.find(profile);
  return it != impl_->profile_keys.end() ? it->second : std::string();
}

RspTabConfig RspConnectionManager::GetConfigForProfile(Profile* profile) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->profile_configs.find(profile);
  return it != impl_->profile_configs.end() ? it->second : RspTabConfig();
}

bool RspConnectionManager::SetConfigForProfile(Profile* profile,
                                               const RspTabConfig& config) {
  if (!profile || config.rm_addr.empty()) {
    return false;
  }

  const std::string new_key = MakeConnectionKey(config);
  std::string old_key;
  bool already_using_config = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto old_it = impl_->profile_keys.find(profile);
    if (old_it != impl_->profile_keys.end()) {
      old_key = old_it->second;
    }
    if (old_it != impl_->profile_keys.end() && old_it->second == new_key &&
        impl_->connections.find(new_key) != impl_->connections.end()) {
      impl_->profile_configs[profile] = config;
      already_using_config = true;
    }
  }
  if (already_using_config) {
    ApplyProxyConfigForProfile(profile, config);
    return true;
  }

  const std::string created_key = GetOrCreate(config);
  if (created_key.empty()) {
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->profile_keys[profile] = std::string();
      impl_->profile_configs[profile] = config;
    }
    ApplyProxyConfigForProfile(profile, config);
    if (!old_key.empty()) {
      Release(old_key);
    }
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->profile_keys[profile] = created_key;
    impl_->profile_configs[profile] = config;
  }
  ApplyProxyConfigForProfile(profile, config);

  if (!old_key.empty() && old_key != created_key) {
    Release(old_key);
  }
  return true;
}

RspConnectionHealth RspConnectionManager::GetHealthForProfile(Profile* profile) {
  RspConnectionHealth health;
  RspTabConfig config;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto config_it = impl_->profile_configs.find(profile);
    if (config_it != impl_->profile_configs.end()) {
      config = config_it->second;
    }
  }

  health.rm_reachable = !config.rm_addr.empty();
  health.rs_configured = !config.rs_node_id.empty();
  if (!health.rm_reachable) {
    health.message = "No RM server configured for this RSP proxy tab";
    return health;
  }
  if (!health.rs_configured) {
    health.message = "Select a bsd_sockets Resource Service Node ID";
    return health;
  }

  health.rs_reachable = false;
  health.message = "Proxy-only mode: RSP reachability probing is disabled";
  return health;
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
    impl_->connections.erase(it);
  }
}

void RspConnectionManager::UnregisterProfile(Profile* profile) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->profile_keys.erase(profile);
  impl_->profile_configs.erase(profile);
}

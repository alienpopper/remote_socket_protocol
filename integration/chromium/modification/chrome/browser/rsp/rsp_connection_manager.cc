// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_connection_manager.h"

#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace {

std::string MakeConnectionKey(const RspTabConfig& config) {
  return base::StringPrintf("%s|%s", config.rm_addr.c_str(),
                            config.rs_node_id.c_str());
}

// Builds the RSP proxy server URI understood by Chromium's net layer.
//
// Format: rsp://<node-id>--<rm-host-dots-as-hyphens>:<rm-port>
// Example: rsp://77c344c4-b564-ddcc-05f2-7be7310f5cfa--127-0-0-1:3939
//
// connect_job_params_factory.cc decodes this by splitting on "--" to recover
// the node ID and RM host:port.  Using dots-as-hyphens avoids embedding bare
// dots in a hostname while keeping the value URL-safe.
std::string MakeRspProxyServer(const RspTabConfig& config) {
  if (config.rs_node_id.empty() || config.rm_addr.empty()) {
    return std::string();
  }

  // Split rm_addr into host and port (format: "host:port").
  std::string rm_host;
  std::string rm_port = "3939";
  size_t colon = config.rm_addr.rfind(':');
  if (colon != std::string::npos) {
    rm_host = config.rm_addr.substr(0, colon);
    rm_port = config.rm_addr.substr(colon + 1);
  } else {
    rm_host = config.rm_addr;
  }

  // Replace dots with hyphens so the combined hostname is a valid DNS label.
  std::string rm_host_hyphened = rm_host;
  for (char& c : rm_host_hyphened) {
    if (c == '.') c = '-';
  }

  return base::StringPrintf("rsp://%s--%s:%s", config.rs_node_id.c_str(),
                            rm_host_hyphened.c_str(), rm_port.c_str());
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

void RspConnectionManager::ListBsdSocketsNodes(const std::string& rm_addr,
                                               NodeListCallback callback) {
#if BUILDFLAG(IS_WIN)
  using ListFn = int (*)(const char*, char***, int*);
  using FreeFn = void (*)(char**, int);

  static HMODULE s_dll = nullptr;
  static ListFn s_list_fn = nullptr;
  static FreeFn s_free_fn = nullptr;
  static bool s_dll_loaded = false;

  if (!s_dll_loaded) {
    s_dll_loaded = true;
    s_dll = ::LoadLibraryExW(L"rspclient.dll", nullptr,
                             LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (s_dll) {
      s_list_fn = reinterpret_cast<ListFn>(
          ::GetProcAddress(s_dll, "rsp_list_bsd_sockets_nodes"));
      s_free_fn = reinterpret_cast<FreeFn>(
          ::GetProcAddress(s_dll, "rsp_free_node_ids"));
    }
  }

  if (!s_list_fn || !s_free_fn) {
    std::move(callback).Run({});
    return;
  }

  ListFn list_fn = s_list_fn;
  FreeFn free_fn = s_free_fn;

  struct NodeLister {
    static std::vector<std::string> Run(ListFn list_fn,
                                        FreeFn free_fn,
                                        std::string rm_addr) {
      char** node_ids = nullptr;
      int count = 0;
      if (!list_fn(rm_addr.c_str(), &node_ids, &count) || count == 0) {
        return {};
      }
      std::vector<std::string> result;
      result.reserve(count);
      for (int i = 0; i < count; ++i) {
        result.emplace_back(node_ids[i]);
      }
      free_fn(node_ids, count);
      return result;
    }
  };

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&NodeLister::Run, list_fn, free_fn, rm_addr),
      std::move(callback));
#else
  std::move(callback).Run({});
#endif
}

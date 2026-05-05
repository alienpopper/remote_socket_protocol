// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/rsp_connect_job.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_event_type.h"
#include "net/log/net_log_source_type.h"
#include "net/socket/socket_descriptor.h"
#include "net/socket/stream_socket.h"
#include "net/socket/tcp_client_socket.h"
#include "net/socket/tcp_socket.h"

#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#endif

namespace net {

// ---------------------------------------------------------------------------
// RspSocketParams
// ---------------------------------------------------------------------------

RspSocketParams::RspSocketParams(
    const std::string& node_id,
    const HostPortPair& rm_host_port,
    const HostPortPair& destination,
    const NetworkAnonymizationKey& network_anonymization_key,
    const NetworkTrafficAnnotationTag& traffic_annotation)
    : node_id_(node_id),
      rm_host_port_(rm_host_port),
      destination_(destination),
      network_anonymization_key_(network_anonymization_key),
      traffic_annotation_(traffic_annotation) {}

RspSocketParams::~RspSocketParams() = default;

// ---------------------------------------------------------------------------
// DLL bridge infrastructure
// ---------------------------------------------------------------------------

namespace {

// Function pointer types matching rsp_c_api.h exported symbols.
using FnCreate = void* (*)(const char*, const char*);
using FnConnectTcp = intptr_t (*)(void*, const char*);
using FnDestroy = void (*)(void*);

struct RspDll {
  bool loaded = false;
  FnCreate create = nullptr;
  FnConnectTcp connect_tcp = nullptr;
  FnDestroy destroy = nullptr;

  bool Load() {
#if BUILDFLAG(IS_WIN)
    // Restrict DLL search to the application directory to avoid hijacking.
    HMODULE handle = ::LoadLibraryExW(
        L"rspclient.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!handle) {
      DLOG(ERROR) << "RSP: LoadLibraryExW(rspclient.dll) failed: "
                  << ::GetLastError();
      return false;
    }
    create = reinterpret_cast<FnCreate>(
        ::GetProcAddress(handle, "rsp_bridge_create"));
    connect_tcp = reinterpret_cast<FnConnectTcp>(
        ::GetProcAddress(handle, "rsp_bridge_connect_tcp"));
    destroy = reinterpret_cast<FnDestroy>(
        ::GetProcAddress(handle, "rsp_bridge_destroy"));
    loaded = create && connect_tcp && destroy;
    if (!loaded) {
      DLOG(ERROR) << "RSP: failed to resolve rsp_bridge_* exports";
      ::FreeLibrary(handle);
    }
    return loaded;
#else
    DLOG(ERROR) << "RSP: rspclient.dll bridge not supported on this platform";
    return false;
#endif
  }
};

// Process-global bridge cache keyed by "rm_addr|rs_node_id".
// Bridges are reused across connections to share the RM transport. On failure
// the stale bridge is evicted so the next attempt creates a fresh one.
struct BridgeCache {
  base::Lock lock;
  std::map<std::string, void*> bridges;
  RspDll dll;

  // Returns a live bridge handle, or nullptr on failure. Thread-safe.
  // IMPORTANT: rsp_bridge_create() is a blocking network operation. We must
  // NOT hold |lock| while calling it or all concurrent connection tasks will
  // serialize behind one thread's blocking connect, causing thread explosion.
  void* GetOrCreate(const std::string& rm_addr, const std::string& rs_node_id) {
    std::string key = rm_addr + "|" + rs_node_id;

    // Fast path: bridge already exists.
    {
      base::AutoLock guard(lock);
      auto it = bridges.find(key);
      if (it != bridges.end()) {
        return it->second;
      }
      if (!dll.loaded && !dll.Load()) {
        return nullptr;
      }
    }

    // Slow path: create a new bridge without holding the lock.
    // A race where two threads both create bridges is benign — the loser's
    // bridge is destroyed immediately after the lock is reacquired.
    void* new_bridge = dll.create(rm_addr.c_str(), rs_node_id.c_str());
    if (!new_bridge) {
      return nullptr;
    }

    {
      base::AutoLock guard(lock);
      auto [it, inserted] = bridges.emplace(key, new_bridge);
      if (!inserted) {
        // Another thread won the race. Destroy ours and use theirs.
        if (dll.destroy) {
          dll.destroy(new_bridge);
        }
        return it->second;
      }
      return new_bridge;
    }
  }

  // Evicts |bridge| and destroys it. Safe to call if bridge is already gone.
  void Evict(const std::string& rm_addr,
             const std::string& rs_node_id,
             void* bridge) {
    std::string key = rm_addr + "|" + rs_node_id;
    void* to_destroy = nullptr;
    {
      base::AutoLock guard(lock);
      auto it = bridges.find(key);
      if (it != bridges.end() && it->second == bridge) {
        to_destroy = it->second;
        bridges.erase(it);
      }
    }
    // Destroy outside the lock so connect_tcp calls in other threads complete.
    if (to_destroy && dll.destroy) {
      dll.destroy(to_destroy);
    }
  }
};

BridgeCache& GetBridgeCache() {
  static base::NoDestructor<BridgeCache> cache;
  return *cache;
}

// Blocking worker running on the thread pool.
// Returns an OS socket (caller must close) or -1 on failure.
intptr_t ConnectViaBridge(std::string rm_addr,
                          std::string rs_node_id,
                          std::string host_port) {
  BridgeCache& cache = GetBridgeCache();
  void* bridge = cache.GetOrCreate(rm_addr, rs_node_id);
  if (!bridge) {
    return -1;
  }
  intptr_t fd = cache.dll.connect_tcp(bridge, host_port.c_str());
  if (fd < 0) {
    // Bridge may be stale (RM disconnected). Evict so the next call recreates.
    cache.Evict(rm_addr, rs_node_id, bridge);
  }
  return fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// RspConnectJob::Factory
// ---------------------------------------------------------------------------

std::unique_ptr<RspConnectJob> RspConnectJob::Factory::Create(
    RequestPriority priority,
    const SocketTag& socket_tag,
    const CommonConnectJobParams* common_connect_job_params,
    scoped_refptr<RspSocketParams> params,
    ConnectJob::Delegate* delegate,
    const NetLogWithSource* net_log) {
  return std::make_unique<RspConnectJob>(priority, socket_tag,
                                         common_connect_job_params,
                                         std::move(params), delegate, net_log);
}

// ---------------------------------------------------------------------------
// RspConnectJob
// ---------------------------------------------------------------------------

RspConnectJob::RspConnectJob(
    RequestPriority priority,
    const SocketTag& socket_tag,
    const CommonConnectJobParams* common_connect_job_params,
    scoped_refptr<RspSocketParams> params,
    ConnectJob::Delegate* delegate,
    const NetLogWithSource* net_log)
    : ConnectJob(priority,
                 socket_tag,
                 base::TimeDelta::Max(),
                 common_connect_job_params,
                 delegate,
                 net_log,
                 NetLogSourceType::RSP_CONNECT_JOB,
                 NetLogEventType::RSP_CONNECT_JOB_CONNECT),
      params_(std::move(params)) {}

RspConnectJob::~RspConnectJob() = default;

LoadState RspConnectJob::GetLoadState() const {
  return LOAD_STATE_ESTABLISHING_PROXY_TUNNEL;
}

bool RspConnectJob::HasEstablishedConnection() const {
  return false;
}

ResolveErrorInfo RspConnectJob::GetResolveErrorInfo() const {
  return resolve_error_info_;
}

void RspConnectJob::OnConnectJobComplete(int result, ConnectJob* job) {
  NOTREACHED();
}

void RspConnectJob::OnNeedsProxyAuth(
    const HttpResponseInfo& response,
    HttpAuthController* auth_controller,
    base::OnceClosure restart_with_auth_callback,
    ConnectJob* job) {
  NOTREACHED();
}

int RspConnectJob::ConnectInternal() {
  next_state_ = STATE_BRIDGE_CONNECT;
  return DoLoop(OK);
}

void RspConnectJob::ChangePriorityInternal(RequestPriority priority) {}

void RspConnectJob::OnIOComplete(int result) {
  int rv = DoLoop(result);
  if (rv != ERR_IO_PENDING) {
    NotifyDelegateOfCompletion(rv);
  }
}

int RspConnectJob::DoLoop(int result) {
  int rv = result;
  do {
    State state = next_state_;
    next_state_ = STATE_NONE;
    switch (state) {
      case STATE_BRIDGE_CONNECT:
        rv = DoBridgeConnect();
        break;
      case STATE_BRIDGE_CONNECT_COMPLETE:
        rv = DoBridgeConnectComplete(rv);
        break;
      default:
        NOTREACHED() << "bad state";
    }
  } while (rv != ERR_IO_PENDING && next_state_ != STATE_NONE);
  return rv;
}

int RspConnectJob::DoBridgeConnect() {
  next_state_ = STATE_BRIDGE_CONNECT_COMPLETE;
  std::string rm_addr = params_->rm_host_port().ToString();
  std::string rs_node_id = params_->node_id();
  std::string host_port = params_->destination().ToString();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ConnectViaBridge, rm_addr, rs_node_id, host_port),
      // If the job is cancelled (weak ptr dead), close the socket here to
      // avoid leaking the OS handle.
      base::BindOnce(
          [](base::WeakPtr<RspConnectJob> self, intptr_t fd) {
            if (!self) {
#if BUILDFLAG(IS_WIN)
              if (fd >= 0) {
                ::closesocket(static_cast<SOCKET>(fd));
              }
#else
              if (fd >= 0) {
                ::close(static_cast<int>(fd));
              }
#endif
              return;
            }
            self->OnBridgeConnected(fd);
          },
          weak_factory_.GetWeakPtr()));

  return ERR_IO_PENDING;
}

void RspConnectJob::OnBridgeConnected(intptr_t fd) {
  bridge_socket_fd_ = fd;
  OnIOComplete(fd >= 0 ? OK : ERR_PROXY_CONNECTION_FAILED);
}

int RspConnectJob::DoBridgeConnectComplete(int result) {
  if (result < 0) {
    return result;
  }

  // Derive a peer IPEndPoint from the RM host. The actual socket connects to
  // the RM so this accurately represents the physical peer for NetLog.
  IPAddress rm_ip;
  if (!rm_ip.AssignFromIPLiteral(params_->rm_host_port().host())) {
    rm_ip = IPAddress::IPv4Localhost();
  }
  IPEndPoint peer_address(rm_ip, params_->rm_host_port().port());

  auto tcp_socket = TCPSocketWin::Create(nullptr, net_log().net_log(),
                                          net_log().source());
  int rv = tcp_socket->AdoptConnectedSocket(
      static_cast<net::SocketDescriptor>(bridge_socket_fd_), peer_address);
  if (rv != OK) {
    // AdoptConnectedSocket closed the socket on failure.
    bridge_socket_fd_ = -1;
    return ERR_PROXY_CONNECTION_FAILED;
  }
  bridge_socket_fd_ = -1;  // Ownership transferred to tcp_socket.

  SetSocket(
      std::make_unique<TCPClientSocket>(std::move(tcp_socket), peer_address),
      std::nullopt);
  return OK;
}

}  // namespace net

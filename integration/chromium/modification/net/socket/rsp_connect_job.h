// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_RSP_CONNECT_JOB_H_
#define NET_SOCKET_RSP_CONNECT_JOB_H_

#include <memory>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/request_priority.h"
#include "net/dns/public/resolve_error_info.h"
#include "net/socket/connect_job.h"
#include "net/socket/stream_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace net {

// Parameters for an RSP proxy connection.
// The DLL (rspclient.dll) handles TCP connect to the RM internally, so no
// nested TransportSocketParams are needed.
class NET_EXPORT_PRIVATE RspSocketParams
    : public base::RefCounted<RspSocketParams> {
 public:
  RspSocketParams(const std::string& node_id,
                  const HostPortPair& rm_host_port,
                  const HostPortPair& destination,
                  const NetworkAnonymizationKey& network_anonymization_key,
                  const NetworkTrafficAnnotationTag& traffic_annotation);

  RspSocketParams(const RspSocketParams&) = delete;
  RspSocketParams& operator=(const RspSocketParams&) = delete;

  const std::string& node_id() const { return node_id_; }
  const HostPortPair& rm_host_port() const { return rm_host_port_; }
  const HostPortPair& destination() const { return destination_; }
  const NetworkAnonymizationKey& network_anonymization_key() const {
    return network_anonymization_key_;
  }
  const NetworkTrafficAnnotationTag& traffic_annotation() const {
    return traffic_annotation_;
  }

 private:
  friend class base::RefCounted<RspSocketParams>;
  ~RspSocketParams();

  const std::string node_id_;
  const HostPortPair rm_host_port_;
  const HostPortPair destination_;
  const NetworkAnonymizationKey network_anonymization_key_;
  const NetworkTrafficAnnotationTag traffic_annotation_;
};

// ConnectJob that establishes an RSP proxy tunnel via rspclient.dll.
// The DLL handles all RSP protocol details (auth, encoding negotiation, etc.)
// and returns a plain OS socket that Chrome adopts as a TCPClientSocket.
class NET_EXPORT_PRIVATE RspConnectJob : public ConnectJob,
                                         public ConnectJob::Delegate {
 public:
  class NET_EXPORT_PRIVATE Factory {
   public:
    Factory() = default;
    virtual ~Factory() = default;

    virtual std::unique_ptr<RspConnectJob> Create(
        RequestPriority priority,
        const SocketTag& socket_tag,
        const CommonConnectJobParams* common_connect_job_params,
        scoped_refptr<RspSocketParams> params,
        ConnectJob::Delegate* delegate,
        const NetLogWithSource* net_log);
  };

  RspConnectJob(RequestPriority priority,
                const SocketTag& socket_tag,
                const CommonConnectJobParams* common_connect_job_params,
                scoped_refptr<RspSocketParams> params,
                ConnectJob::Delegate* delegate,
                const NetLogWithSource* net_log);

  RspConnectJob(const RspConnectJob&) = delete;
  RspConnectJob& operator=(const RspConnectJob&) = delete;
  ~RspConnectJob() override;

  // ConnectJob:
  LoadState GetLoadState() const override;
  bool HasEstablishedConnection() const override;
  ResolveErrorInfo GetResolveErrorInfo() const override;

 private:
  enum State {
    STATE_BRIDGE_CONNECT,
    STATE_BRIDGE_CONNECT_COMPLETE,
    STATE_NONE,
  };

  // ConnectJob::Delegate (unused — RspConnectJob has no nested ConnectJobs):
  void OnConnectJobComplete(int result, ConnectJob* job) override;
  void OnNeedsProxyAuth(const HttpResponseInfo& response,
                        HttpAuthController* auth_controller,
                        base::OnceClosure restart_with_auth_callback,
                        ConnectJob* job) override;

  // ConnectJob:
  int ConnectInternal() override;
  void ChangePriorityInternal(RequestPriority priority) override;

  void OnIOComplete(int result);
  int DoLoop(int result);
  int DoBridgeConnect();
  int DoBridgeConnectComplete(int result);
  // Called on I/O thread with the OS socket from rsp_bridge_connect_tcp.
  // If the job was cancelled the socket is closed here to avoid leaking it.
  void OnBridgeConnected(intptr_t fd);

  scoped_refptr<RspSocketParams> params_;
  State next_state_ = STATE_NONE;
  intptr_t bridge_socket_fd_ = -1;
  ResolveErrorInfo resolve_error_info_;
  base::WeakPtrFactory<RspConnectJob> weak_factory_{this};
};

}  // namespace net

#endif  // NET_SOCKET_RSP_CONNECT_JOB_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_url_loader_factory.h"

#include <string>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/rsp/rsp_connection_manager.h"
#include "chrome/browser/rsp/rsp_http_helper.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace rsp {

namespace {

class RspHTTPURLLoader : public network::mojom::URLLoader {
 public:
  RspHTTPURLLoader(std::string connection_key,
                   network::ResourceRequest request,
                   mojo::PendingRemote<network::mojom::URLLoaderClient> client)
      : connection_key_(std::move(connection_key)),
        request_(std::move(request)),
        client_(std::move(client)) {}

  ~RspHTTPURLLoader() override = default;

  void Start() {
    const std::string host = std::string(request_.url.host());
    const int port = request_.url.EffectiveIntPort();
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&RspHTTPURLLoader::DoConnect,
                       connection_key_,
                       std::move(host),
                       port),
        base::BindOnce(&RspHTTPURLLoader::OnConnected,
                       weak_factory_.GetWeakPtr()));
  }

  void FollowRedirect(const std::vector<std::string>&,
                      const net::HttpRequestHeaders&,
                      const net::HttpRequestHeaders&,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}

 private:
  static int DoConnect(std::string connection_key, std::string host, int port) {
    const std::string host_port =
        base::StringPrintf("%s:%d", host.c_str(), port);
    return RspConnectionManager::GetInstance()->ConnectTCPSocket(
        connection_key, host_port);
  }

  void OnConnected(int fd) {
    if (fd < 0) {
      client_->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_CONNECTION_FAILED));
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&DoHttpRequest, fd, request_),
        base::BindOnce(&RspHTTPURLLoader::OnHttpDone,
                       weak_factory_.GetWeakPtr()));
  }

  void OnHttpDone(HttpResult result) {
    if (result.net_error != net::OK) {
      client_->OnComplete(
          network::URLLoaderCompletionStatus(result.net_error));
      return;
    }

    mojo::ScopedDataPipeConsumerHandle consumer;
    mojo::ScopedDataPipeProducerHandle producer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
      client_->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
      return;
    }

    if (!result.body.empty()) {
      auto body_bytes = base::as_bytes(base::span(result.body));
      size_t bytes_written = 0;
      producer->WriteData(body_bytes, MOJO_WRITE_DATA_FLAG_NONE, bytes_written);
    }
    producer.reset();

    auto head = network::mojom::URLResponseHead::New();
    head->headers = result.headers;
    if (result.headers) {
      result.headers->GetMimeType(&head->mime_type);
      result.headers->GetCharset(&head->charset);
    }

    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    client_->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  }

  const std::string connection_key_;
  const network::ResourceRequest request_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  base::WeakPtrFactory<RspHTTPURLLoader> weak_factory_{this};
};

class RspInterceptingURLLoaderFactory
    : public network::mojom::URLLoaderFactory {
 public:
  RspInterceptingURLLoaderFactory(
      std::string connection_key,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> next_factory,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      : connection_key_(std::move(connection_key)),
        next_factory_(std::move(next_factory)),
        receiver_(this, std::move(receiver)) {}

  ~RspInterceptingURLLoaderFactory() override = default;

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    if (!request.url.SchemeIs("http")) {
      next_factory_->CreateLoaderAndStart(std::move(loader), request_id,
                                          options, request, std::move(client),
                                          traffic_annotation);
      return;
    }

    DVLOG(1) << "RspInterceptingURLLoaderFactory: intercepting "
             << request.url.spec();
    auto rsp_loader = std::make_unique<RspHTTPURLLoader>(
        connection_key_, request, std::move(client));
    auto* ptr = rsp_loader.get();
    mojo::MakeSelfOwnedReceiver(std::move(rsp_loader), std::move(loader));
    ptr->Start();
  }

  void Clone(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  const std::string connection_key_;
  mojo::Remote<network::mojom::URLLoaderFactory> next_factory_;
  mojo::Receiver<network::mojom::URLLoaderFactory> receiver_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
};

}  // namespace

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

  auto* mgr = RspConnectionManager::GetInstance();
  const std::string connection_key = mgr->GetKeyForProfile(profile);
  if (connection_key.empty()) {
    DVLOG(1) << "RspURLLoaderFactory: no connection registered for profile "
             << static_cast<void*>(profile);
    return;
  }

  mojo::PendingReceiver<network::mojom::URLLoaderFactory> our_receiver;
  mojo::PendingRemote<network::mojom::URLLoaderFactory> next_remote;
  std::tie(our_receiver, next_remote) = factory_builder.Append();
  new RspInterceptingURLLoaderFactory(connection_key, std::move(next_remote),
                                      std::move(our_receiver));
}

}  // namespace rsp

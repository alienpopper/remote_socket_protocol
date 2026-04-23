// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_scheme_url_loader_factory.h"

#include <stdint.h>
#include <string>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/rsp/rsp_config.h"
#include "chrome/browser/rsp/rsp_connection_manager.h"
#include "chrome/browser/rsp/rsp_http_helper.h"
#include "chrome/browser/rsp/rsp_prefs.h"
#include "components/prefs/pref_service.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/browser_context.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace rsp {

namespace {

// Handles a single rsp://host[:port]/path request by opening a TCP socket
// through the bsd_sockets RS and performing a plain HTTP/1.1 request.
class RspSchemeURLLoader : public network::mojom::URLLoader {
 public:
  RspSchemeURLLoader(
      std::string connection_key,
      network::ResourceRequest request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client)
      : connection_key_(std::move(connection_key)),
        request_(std::move(request)),
        client_(std::move(client)) {}

  ~RspSchemeURLLoader() override = default;

  void Start() {
    const std::string host = std::string(request_.url.host());
    const int port = request_.url.EffectiveIntPort();
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&RspSchemeURLLoader::DoConnect, connection_key_, host,
                       port),
        base::BindOnce(&RspSchemeURLLoader::OnConnected,
                       weak_factory_.GetWeakPtr()));
  }

  void FollowRedirect(const std::vector<std::string>&,
                      const net::HttpRequestHeaders&,
                      const net::HttpRequestHeaders&,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}

 private:
  static intptr_t DoConnect(const std::string& connection_key,
                            const std::string& host,
                            int port) {
    const std::string host_port =
        base::StringPrintf("%s:%d", host.c_str(), port);
    return RspConnectionManager::GetInstance()->ConnectTCPSocket(connection_key,
                                                                 host_port);
  }

  void OnConnected(intptr_t socket) {
    if (socket < 0) {
      client_->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_CONNECTION_FAILED));
      return;
    }
    // Rewrite rsp:// → http:// so the HTTP helper sends the right scheme.
    network::ResourceRequest http_request = request_;
    GURL::Replacements rep;
    rep.SetSchemeStr("http");
    http_request.url = request_.url.ReplaceComponents(rep);

    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&DoHttpRequest, socket, std::move(http_request)),
        base::BindOnce(&RspSchemeURLLoader::OnHttpDone,
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
      size_t bytes_written = 0;
      producer->WriteData(base::as_bytes(base::span(result.body)),
                          MOJO_WRITE_DATA_FLAG_NONE, bytes_written);
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
  base::WeakPtrFactory<RspSchemeURLLoader> weak_factory_{this};
};

}  // namespace

// static
void RspSchemeURLLoaderFactory::Create(
    content::BrowserContext* browser_context,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver) {
  auto* prefs = user_prefs::UserPrefs::Get(browser_context);
  const std::string rm_addr = prefs->GetString(prefs::kRspDefaultRmAddr);
  const std::string rs_node_id = prefs->GetString(prefs::kRspDefaultRsNodeId);

  std::string connection_key;
  if (!rm_addr.empty() && !rs_node_id.empty()) {
    RspTabConfig config;
    config.rm_addr = rm_addr;
    config.rs_node_id = rs_node_id;
    connection_key = RspConnectionManager::GetInstance()->GetOrCreate(config);
  }

  new RspSchemeURLLoaderFactory(std::move(connection_key),
                                std::move(factory_receiver));
}

RspSchemeURLLoaderFactory::RspSchemeURLLoaderFactory(
    std::string connection_key,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver)
    : SelfDeletingURLLoaderFactory(std::move(factory_receiver)),
      connection_key_(std::move(connection_key)) {}

RspSchemeURLLoaderFactory::~RspSchemeURLLoaderFactory() = default;

void RspSchemeURLLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  if (connection_key_.empty()) {
    DVLOG(1) << "RspSchemeURLLoaderFactory: no default RSP connection. "
                "Set RM address and RS node ID in chrome://settings/rsp";
    mojo::Remote<network::mojom::URLLoaderClient> c(std::move(client));
    c->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_FAILED));
    return;
  }

  DVLOG(1) << "RspSchemeURLLoaderFactory: routing " << request.url.spec();
  auto rsp_loader = std::make_unique<RspSchemeURLLoader>(connection_key_,
                                                         request,
                                                         std::move(client));
  auto* ptr = rsp_loader.get();
  mojo::MakeSelfOwnedReceiver(std::move(rsp_loader), std::move(loader));
  ptr->Start();
}

}  // namespace rsp

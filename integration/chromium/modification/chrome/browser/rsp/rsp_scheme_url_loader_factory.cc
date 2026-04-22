// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_scheme_url_loader_factory.h"

#include "base/logging.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"

namespace rsp {

namespace {

// A URLLoader that immediately fails with ERR_NOT_IMPLEMENTED.
// Self-owned via mojo::MakeSelfOwnedReceiver.
class RspSchemeURLLoader : public network::mojom::URLLoader {
 public:
  RspSchemeURLLoader() = default;
  ~RspSchemeURLLoader() override = default;

  void Start(mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote,
             const GURL& url) {
    mojo::Remote<network::mojom::URLLoaderClient> client(
        std::move(client_remote));
    DVLOG(1) << "RspSchemeURLLoader: rejecting rsp:// request (stub): "
             << url.spec();
    client->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_NOT_IMPLEMENTED));
  }

  // network::mojom::URLLoader:
  void FollowRedirect(
      const std::vector<std::string>& removed_headers,
      const net::HttpRequestHeaders& modified_headers,
      const net::HttpRequestHeaders& modified_cors_exempt_headers,
      const std::optional<GURL>& new_url) override {}
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}
};

}  // namespace

// static
void RspSchemeURLLoaderFactory::Create(
    content::BrowserContext* browser_context,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver) {
  new RspSchemeURLLoaderFactory(std::move(factory_receiver));
}

RspSchemeURLLoaderFactory::RspSchemeURLLoaderFactory(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver)
    : SelfDeletingURLLoaderFactory(std::move(factory_receiver)) {}

RspSchemeURLLoaderFactory::~RspSchemeURLLoaderFactory() = default;

void RspSchemeURLLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  auto rsp_loader = std::make_unique<RspSchemeURLLoader>();
  auto* rsp_loader_ptr = rsp_loader.get();
  mojo::MakeSelfOwnedReceiver(std::move(rsp_loader), std::move(loader));
  rsp_loader_ptr->Start(std::move(client), request.url);
}

}  // namespace rsp

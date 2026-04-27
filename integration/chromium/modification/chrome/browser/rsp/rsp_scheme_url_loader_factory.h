// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_SCHEME_URL_LOADER_FACTORY_H_
#define CHROME_BROWSER_RSP_RSP_SCHEME_URL_LOADER_FACTORY_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace content {
class BrowserContext;
}

namespace rsp {

// URLLoaderFactory for the rsp:// scheme.
// rsp://<node-id>/... opens a plain HTTP stream to an RSP httpd Resource
// Service node and sends HTTP/1.1 over RSP. TLS is intentionally not used.
class RspSchemeURLLoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  // Creates a factory and binds it to |factory_receiver|.  The factory
  // self-deletes when all receivers disconnect.
  static void Create(
      content::BrowserContext* browser_context,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver);

  RspSchemeURLLoaderFactory(const RspSchemeURLLoaderFactory&) = delete;
  RspSchemeURLLoaderFactory& operator=(const RspSchemeURLLoaderFactory&) =
      delete;

 private:
  explicit RspSchemeURLLoaderFactory(
      std::string connection_key,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver);
  ~RspSchemeURLLoaderFactory() override;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override;

  const std::string connection_key_;
};

}  // namespace rsp

#endif  // CHROME_BROWSER_RSP_RSP_SCHEME_URL_LOADER_FACTORY_H_

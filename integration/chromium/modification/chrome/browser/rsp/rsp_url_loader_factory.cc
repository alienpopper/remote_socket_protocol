// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_url_loader_factory.h"

#include <array>
#include <string>
#include <unistd.h>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/rsp/rsp_connection_manager.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace rsp {

namespace {

struct HttpResult {
  int net_error = net::ERR_FAILED;
  scoped_refptr<net::HttpResponseHeaders> headers;
  std::string body;
};

std::string HostOf(const GURL& url) {
  return std::string(url.host());
}

std::string BuildHttpRequest(const network::ResourceRequest& request) {
  const GURL& url = request.url;
  std::string out = base::StringPrintf("%s %s HTTP/1.1\r\n",
                                       request.method.c_str(),
                                       url.PathForRequest().c_str());
  out += "Host: " + HostOf(url) + "\r\n";
  out += "Connection: close\r\n";

  net::HttpRequestHeaders::Iterator it(request.headers);
  while (it.GetNext()) {
    if (base::EqualsCaseInsensitiveASCII(it.name(), "host") ||
        base::EqualsCaseInsensitiveASCII(it.name(), "connection")) {
      continue;
    }
    out += it.name() + ": " + it.value() + "\r\n";
  }
  out += "\r\n";
  return out;
}

std::string ReadUntil(int fd, const std::string& delimiter) {
  std::string buf;
  std::array<char, 4096> tmp;
  while (buf.find(delimiter) == std::string::npos) {
    ssize_t n = ::read(fd, tmp.data(), tmp.size());
    if (n <= 0) {
      return std::string();
    }
    buf.append(tmp.data(), static_cast<size_t>(n));
  }
  return buf.substr(0, buf.find(delimiter) + delimiter.size());
}

void ReadAll(int fd, std::string& out) {
  std::array<char, 4096> tmp;
  while (true) {
    ssize_t n = ::read(fd, tmp.data(), tmp.size());
    if (n <= 0) {
      break;
    }
    out.append(tmp.data(), static_cast<size_t>(n));
  }
}

HttpResult DoHttpRequest(int fd, network::ResourceRequest request) {
  HttpResult result;
  const std::string req_str = BuildHttpRequest(request);

  // Use span to avoid raw pointer arithmetic (Chromium unsafe-buffer policy).
  auto remaining = base::as_bytes(base::span(req_str));
  while (!remaining.empty()) {
    ssize_t n = ::write(fd, remaining.data(),
                        static_cast<size_t>(remaining.size()));
    if (n <= 0) {
      result.net_error = net::ERR_CONNECTION_RESET;
      ::close(fd);
      return result;
    }
    remaining = remaining.subspan(static_cast<size_t>(n));
  }

  const std::string header_text = ReadUntil(fd, "\r\n\r\n");
  if (header_text.empty()) {
    result.net_error = net::ERR_EMPTY_RESPONSE;
    ::close(fd);
    return result;
  }

  const std::string raw = net::HttpUtil::AssembleRawHeaders(header_text);
  result.headers = base::MakeRefCounted<net::HttpResponseHeaders>(raw);

  ReadAll(fd, result.body);
  ::close(fd);
  result.net_error = net::OK;
  return result;
}

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
    const std::string host = HostOf(request_.url);
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

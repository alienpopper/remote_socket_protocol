#pragma once

#include "messages.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rsp::resource_manager {

// An immutable point-in-time copy of schema data.
// Constructed under the SchemaRegistry lock so that evaluations
// against it see a consistent set of descriptors.
// Lazily builds a DescriptorPool + DynamicMessageFactory on first use.
class SchemaSnapshot {
public:
    SchemaSnapshot() : inner_(std::make_shared<Inner>()) {}
    explicit SchemaSnapshot(std::vector<rsp::proto::ServiceSchema> schemas)
        : inner_(std::make_shared<Inner>(std::move(schemas))) {}

    // Find a schema whose accepted_type_urls contains the given type_url.
    std::optional<rsp::proto::ServiceSchema> findByTypeUrl(const std::string& typeUrl) const {
        for (const auto& schema : inner_->schemas) {
            for (const auto& url : schema.accepted_type_urls()) {
                if (url == typeUrl) return schema;
            }
        }
        return std::nullopt;
    }

    // Get a Descriptor for the given full message type name (e.g. "rsp.proto.MyMsg").
    // Lazily builds the DescriptorPool on first call.
    const google::protobuf::Descriptor* findMessageDescriptor(const std::string& fullTypeName) const {
        inner_->ensurePool();
        if (!inner_->pool) return nullptr;
        return inner_->pool->FindMessageTypeByName(fullTypeName);
    }

    // Get the DynamicMessageFactory (lazily built).
    google::protobuf::DynamicMessageFactory* messageFactory() const {
        inner_->ensurePool();
        return inner_->factory.get();
    }

    bool empty() const { return inner_->schemas.empty(); }

private:
    struct Inner {
        std::vector<rsp::proto::ServiceSchema> schemas;
        mutable std::once_flag poolBuilt;
        mutable std::unique_ptr<google::protobuf::DescriptorPool> pool;
        mutable std::unique_ptr<google::protobuf::DynamicMessageFactory> factory;

        Inner() = default;
        explicit Inner(std::vector<rsp::proto::ServiceSchema> s) : schemas(std::move(s)) {}

        void ensurePool() const {
            std::call_once(poolBuilt, [this]() {
                if (schemas.empty()) return;
                pool = std::make_unique<google::protobuf::DescriptorPool>();
                for (const auto& schema : schemas) {
                    google::protobuf::FileDescriptorSet fds;
                    if (!fds.ParseFromString(schema.proto_file_descriptor_set())) continue;
                    for (const auto& fileProto : fds.file()) {
                        if (pool->FindFileByName(fileProto.name()) != nullptr) continue;
                        pool->BuildFile(fileProto);
                    }
                }
                factory = std::make_unique<google::protobuf::DynamicMessageFactory>(pool.get());
            });
        }
    };

    std::shared_ptr<Inner> inner_;
};

// Content-addressed schema store.  Schemas are deduplicated by their
// SHA-256 hash (schema_hash) and indexed by proto_file_name for lookup.
// Thread-safe via an internal mutex.
class SchemaRegistry {
public:
    // Ingest all schemas from an advertisement.  Duplicates (same hash)
    // are silently ignored.
    void ingest(const rsp::proto::ResourceAdvertisement& advertisement) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& schema : advertisement.schemas()) {
            const std::string& hash = schema.schema_hash();
            if (hash.empty() || byHash_.count(hash)) {
                continue;
            }
            byHash_[hash] = schema;
            byProtoFile_[schema.proto_file_name()] = hash;
        }
    }

    // Look up all known schemas (deduplicated).
    std::vector<rsp::proto::ServiceSchema> allSchemas() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<rsp::proto::ServiceSchema> result;
        result.reserve(byHash_.size());
        for (const auto& [_, schema] : byHash_) {
            result.push_back(schema);
        }
        return result;
    }

    // Look up a schema by its proto_file_name.
    std::optional<rsp::proto::ServiceSchema> findByProtoFile(const std::string& protoFile) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = byProtoFile_.find(protoFile);
        if (it == byProtoFile_.end()) return std::nullopt;
        auto hashIt = byHash_.find(it->second);
        if (hashIt == byHash_.end()) return std::nullopt;
        return hashIt->second;
    }

    // Look up a schema by its hash.
    std::optional<rsp::proto::ServiceSchema> findByHash(const std::string& hash) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = byHash_.find(hash);
        if (it == byHash_.end()) return std::nullopt;
        return it->second;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return byHash_.size();
    }

    // Take an immutable snapshot of all schemas.  The snapshot is safe to
    // use without holding any locks, guaranteeing evaluation atomicity.
    SchemaSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<rsp::proto::ServiceSchema> schemas;
        schemas.reserve(byHash_.size());
        for (const auto& [_, schema] : byHash_) {
            schemas.push_back(schema);
        }
        return SchemaSnapshot(std::move(schemas));
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, rsp::proto::ServiceSchema> byHash_;       // hash -> schema
    std::map<std::string, std::string>                byProtoFile_;  // proto_file_name -> hash
};

}  // namespace rsp::resource_manager

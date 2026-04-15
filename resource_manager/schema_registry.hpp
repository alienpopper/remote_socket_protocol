#pragma once

#include "messages.pb.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rsp::resource_manager {

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

private:
    mutable std::mutex mutex_;
    std::map<std::string, rsp::proto::ServiceSchema> byHash_;       // hash -> schema
    std::map<std::string, std::string>                byProtoFile_;  // proto_file_name -> hash
};

}  // namespace rsp::resource_manager

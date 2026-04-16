#pragma once

#include "resource_service/resource_service.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace rsp::name_service {

class NameService : public rsp::resource_service::ResourceService {
public:
    using Ptr = std::shared_ptr<NameService>;

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~NameService() override;

    NameService(const NameService&) = delete;
    NameService& operator=(const NameService&) = delete;
    NameService(NameService&&) = delete;
    NameService& operator=(NameService&&) = delete;

protected:
    explicit NameService(KeyPair keyPair);

    rsp::proto::ResourceAdvertisement buildResourceAdvertisement() const override;
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;

private:
    // Key: (name, owner_bytes, type_bytes)
    using RecordKey = std::tuple<std::string, std::string, std::string>;

public:
    struct StoredRecord {
        std::string name;
        std::string ownerBytes;   // serialised NodeId.value
        std::string typeBytes;    // serialised Uuid.value
        std::string valueBytes;   // serialised Uuid.value
    };

private:

    mutable std::mutex dbMutex_;
    std::map<RecordKey, StoredRecord> records_;

    // Handlers
    bool handleCreateRequest(const rsp::proto::RSPMessage& message);
    bool handleReadRequest(const rsp::proto::RSPMessage& message);
    bool handleUpdateRequest(const rsp::proto::RSPMessage& message);
    bool handleDeleteRequest(const rsp::proto::RSPMessage& message);
    bool handleQueryRequest(const rsp::proto::RSPMessage& message);
};

}  // namespace rsp::name_service

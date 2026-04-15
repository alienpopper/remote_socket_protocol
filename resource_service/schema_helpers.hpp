#pragma once

#include "messages.pb.h"
#include <openssl/sha.h>
#include <cstddef>
#include <string>
#include <vector>

namespace rsp::resource_service {

// Build a ServiceSchema from an embedded FileDescriptorSet and a list of
// accepted type URLs.  The schema_hash is computed as SHA-256 of the raw
// descriptor bytes, which provides content-based deduplication at the RM.
inline rsp::proto::ServiceSchema buildServiceSchema(
        const char* protoFileName,
        const unsigned char* descriptorData,
        size_t descriptorSize,
        uint32_t schemaVersion,
        const std::vector<std::string>& acceptedTypeUrls) {
    rsp::proto::ServiceSchema schema;
    schema.set_proto_file_name(protoFileName);
    schema.set_proto_file_descriptor_set(descriptorData, descriptorSize);
    schema.set_schema_version(schemaVersion);

    // Compute SHA-256 of the descriptor bytes for content-based dedup
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(descriptorData, descriptorSize, hash);
    schema.set_schema_hash(hash, SHA256_DIGEST_LENGTH);

    for (const auto& url : acceptedTypeUrls) {
        schema.add_accepted_type_urls(url);
    }

    return schema;
}

}  // namespace rsp::resource_service

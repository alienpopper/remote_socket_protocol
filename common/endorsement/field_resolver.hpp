#pragma once

#include "messages.pb.h"

#include <google/protobuf/any.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rsp::resource_manager {
class SchemaSnapshot;
}

namespace rsp::endorsement {

// The set of value types that can be resolved from a field path.
// Matches the ERDASTFieldValue oneof alternatives.
using ResolvedValue = std::variant<
    std::monostate,       // field not present / resolution failed
    std::string,          // bytes_value or string_value  (bytes are stored as std::string)
    int64_t,              // int_value (int32/sint32/int64/sint64/sfixed32/sfixed64)
    uint64_t,             // uint_value (uint32/uint64/fixed32/fixed64)
    bool,                 // bool_value
    int32_t               // enum_value (protobuf enum ordinal)
>;

// Resolve a field path against an RSPMessage.
//
// path segments are walked left to right.  For most segments the
// protobuf Reflection API (compiled descriptor) is used.  If a segment
// names the "service_message" Any field, the schema snapshot is used to
// build a DynamicMessage, and subsequent segments walk into that message.
//
// Returns std::monostate if the path cannot be resolved (missing field,
// unknown schema, etc.).
//
// schemaSnapshot may be nullptr if no runtime schemas are available;
// resolution of service_message sub-paths will then always return
// std::monostate.
ResolvedValue resolveFieldPath(
    const rsp::proto::ERDASTFieldPath& path,
    const google::protobuf::Message& message,
    const rsp::resource_manager::SchemaSnapshot* schemaSnapshot);

// Compare a ResolvedValue against an ERDASTFieldValue literal.
bool resolvedValueEquals(const ResolvedValue& resolved,
                         const rsp::proto::ERDASTFieldValue& expected);

// Check whether a resolved value indicates "present" (not monostate).
bool resolvedValuePresent(const ResolvedValue& resolved);

// Resolve a field path and collect all elements of a repeated field.
// If the path points to a repeated field, returns a vector of ResolvedValues
// (one per element).  For scalars or missing fields returns an empty vector.
std::vector<const google::protobuf::Message*> resolveRepeatedMessages(
    const rsp::proto::ERDASTFieldPath& path,
    const google::protobuf::Message& message,
    const rsp::resource_manager::SchemaSnapshot* schemaSnapshot);

}  // namespace rsp::endorsement

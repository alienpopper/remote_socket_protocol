#include "common/endorsement/field_resolver.hpp"
#include "resource_manager/schema_registry.hpp"

#include <google/protobuf/any.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <memory>
#include <string>

namespace {

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;

// Extract a type name from a type_url.  "type.rsp/rsp.proto.Foo" → "rsp.proto.Foo".
std::string typeNameFromUrl(const std::string& typeUrl) {
    const auto pos = typeUrl.rfind('/');
    if (pos == std::string::npos) return typeUrl;
    return typeUrl.substr(pos + 1);
}

// Read a scalar field value into a ResolvedValue.
rsp::endorsement::ResolvedValue readScalar(const Message& msg,
                                           const FieldDescriptor* field,
                                           const Reflection* ref) {
    switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING: {
            std::string scratch;
            const std::string& val = ref->GetStringReference(msg, field, &scratch);
            return val;
        }
        case FieldDescriptor::CPPTYPE_INT32:
            return static_cast<int64_t>(ref->GetInt32(msg, field));
        case FieldDescriptor::CPPTYPE_INT64:
            return ref->GetInt64(msg, field);
        case FieldDescriptor::CPPTYPE_UINT32:
            return static_cast<uint64_t>(ref->GetUInt32(msg, field));
        case FieldDescriptor::CPPTYPE_UINT64:
            return ref->GetUInt64(msg, field);
        case FieldDescriptor::CPPTYPE_BOOL:
            return ref->GetBool(msg, field);
        case FieldDescriptor::CPPTYPE_ENUM:
            return static_cast<int32_t>(ref->GetEnum(msg, field)->number());
        case FieldDescriptor::CPPTYPE_FLOAT:
            return static_cast<int64_t>(ref->GetFloat(msg, field));
        case FieldDescriptor::CPPTYPE_DOUBLE:
            return static_cast<int64_t>(ref->GetDouble(msg, field));
        case FieldDescriptor::CPPTYPE_MESSAGE:
            // Sub-messages can't be returned as a scalar.
            return std::monostate{};
    }
    return std::monostate{};
}

// Given a message and the remaining path segments (starting at `startIdx`),
// walk into sub-messages.  Returns nullptr if the path can't be followed.
// On success, sets `outField` to the terminal field and `outMsg` to the
// message that contains it.
struct WalkResult {
    const Message* message = nullptr;
    const FieldDescriptor* field = nullptr;
};

WalkResult walkPath(const Message& msg,
                    const rsp::proto::ERDASTFieldPath& path,
                    int startIdx,
                    const rsp::resource_manager::SchemaSnapshot* snap,
                    std::unique_ptr<Message>& dynamicHolder) {
    const Message* current = &msg;
    const Descriptor* desc = current->GetDescriptor();
    const Reflection* ref = current->GetReflection();

    for (int i = startIdx; i < path.segments_size(); ++i) {
        const std::string& seg = path.segments(i);

        const FieldDescriptor* field = desc->FindFieldByName(seg);
        if (!field) return {};

        // Terminal segment?
        if (i == path.segments_size() - 1) {
            return {current, field};
        }

        // Must be a sub-message to continue walking.
        if (field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) return {};

        // Special case: if the field is a google.protobuf.Any and we have a schema
        // snapshot, unpack it into a DynamicMessage and continue walking.
        if (field->message_type()->full_name() == "google.protobuf.Any" && snap) {
            const Message& anyMsg = ref->GetMessage(*current, field);
            const auto* anyDesc = anyMsg.GetDescriptor();
            const auto* anyRef = anyMsg.GetReflection();
            const auto* typeUrlField = anyDesc->FindFieldByName("type_url");
            const auto* valueField = anyDesc->FindFieldByName("value");
            if (!typeUrlField || !valueField) return {};

            const std::string typeUrl = anyRef->GetString(anyMsg, typeUrlField);
            const std::string value = anyRef->GetString(anyMsg, valueField);
            const std::string typeName = typeNameFromUrl(typeUrl);

            const Descriptor* dynDesc = snap->findMessageDescriptor(typeName);
            if (!dynDesc) return {};
            auto* factory = snap->messageFactory();
            if (!factory) return {};

            const Message* prototype = factory->GetPrototype(dynDesc);
            if (!prototype) return {};
            dynamicHolder.reset(prototype->New());
            if (!dynamicHolder->ParseFromString(value)) return {};

            current = dynamicHolder.get();
            desc = current->GetDescriptor();
            ref = current->GetReflection();
            continue;
        }

        // Normal sub-message.
        current = &ref->GetMessage(*current, field);
        desc = current->GetDescriptor();
        ref = current->GetReflection();
    }

    return {};
}

}  // namespace

namespace rsp::endorsement {

ResolvedValue resolveFieldPath(
    const rsp::proto::ERDASTFieldPath& path,
    const google::protobuf::Message& message,
    const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {

    if (path.segments_size() == 0) return std::monostate{};

    std::unique_ptr<Message> dynamicHolder;
    auto result = walkPath(message, path, 0, schemaSnapshot, dynamicHolder);
    if (!result.message || !result.field) return std::monostate{};

    const Reflection* ref = result.message->GetReflection();

    if (result.field->is_repeated()) {
        // Repeated fields: can't return as a single scalar.
        return std::monostate{};
    }

    // For optional/singular message fields, check presence.
    if (result.field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
        if (!ref->HasField(*result.message, result.field)) return std::monostate{};
        return std::monostate{};  // sub-messages aren't scalars
    }

    return readScalar(*result.message, result.field, ref);
}

bool resolvedValueEquals(const ResolvedValue& resolved,
                         const rsp::proto::ERDASTFieldValue& expected) {
    if (std::holds_alternative<std::monostate>(resolved)) return false;

    switch (expected.value_case()) {
        case rsp::proto::ERDASTFieldValue::kBytesValue:
            if (auto* s = std::get_if<std::string>(&resolved))
                return *s == expected.bytes_value();
            return false;

        case rsp::proto::ERDASTFieldValue::kStringValue:
            if (auto* s = std::get_if<std::string>(&resolved))
                return *s == expected.string_value();
            return false;

        case rsp::proto::ERDASTFieldValue::kIntValue:
            if (auto* v = std::get_if<int64_t>(&resolved))
                return *v == expected.int_value();
            return false;

        case rsp::proto::ERDASTFieldValue::kUintValue:
            if (auto* v = std::get_if<uint64_t>(&resolved))
                return *v == expected.uint_value();
            return false;

        case rsp::proto::ERDASTFieldValue::kBoolValue:
            if (auto* v = std::get_if<bool>(&resolved))
                return *v == expected.bool_value();
            return false;

        case rsp::proto::ERDASTFieldValue::kEnumValue:
            if (auto* v = std::get_if<int32_t>(&resolved))
                return *v == expected.enum_value();
            return false;

        case rsp::proto::ERDASTFieldValue::VALUE_NOT_SET:
            return false;
    }
    return false;
}

bool resolvedValuePresent(const ResolvedValue& resolved) {
    return !std::holds_alternative<std::monostate>(resolved);
}

std::vector<const google::protobuf::Message*> resolveRepeatedMessages(
    const rsp::proto::ERDASTFieldPath& path,
    const google::protobuf::Message& message,
    const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {

    if (path.segments_size() == 0) return {};

    std::unique_ptr<Message> dynamicHolder;
    auto result = walkPath(message, path, 0, schemaSnapshot, dynamicHolder);
    if (!result.message || !result.field) return {};

    if (!result.field->is_repeated()) return {};
    if (result.field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) return {};

    const Reflection* ref = result.message->GetReflection();
    const int count = ref->FieldSize(*result.message, result.field);
    std::vector<const Message*> elements;
    elements.reserve(count);
    for (int i = 0; i < count; ++i) {
        elements.push_back(&ref->GetRepeatedMessage(*result.message, result.field, i));
    }
    return elements;
}

}  // namespace rsp::endorsement

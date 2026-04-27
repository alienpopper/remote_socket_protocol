// Transcoding test: demonstrates an RM loading a .proto at runtime and
// translating between protobuf binary and JSON for dynamically-defined
// messages.  An RS that only speaks protobuf can talk to a client that
// only speaks JSON because the RM acts as a transcoding bridge.

#include "common/service_message.hpp"
#include "messages.pb.h"

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <openssl/sha.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

bool testsPassed = true;
int testsRun = 0;

void require(bool condition, const std::string& description) {
    ++testsRun;
    if (!condition) {
        std::cerr << "FAIL: " << description << "\n";
        testsPassed = false;
    }
}

class SilentErrorCollector : public google::protobuf::compiler::MultiFileErrorCollector {
public:
    void RecordError(absl::string_view filename, int line, int column, absl::string_view message) override {
        errors_.emplace_back(std::string(filename) + ":" + std::to_string(line) + ":" +
                             std::to_string(column) + ": " + std::string(message));
    }
    void RecordWarning(absl::string_view, int, int, absl::string_view) override {}
    bool hasErrors() const { return !errors_.empty(); }
    void printErrors() const {
        for (const auto& e : errors_) std::cerr << "  proto error: " << e << "\n";
    }
private:
    std::vector<std::string> errors_;
};

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// Compute SHA-256 over the deterministic serialization of a message.
// This is the generic approach the RM would use for dynamically-loaded
// messages: serialize to canonical protobuf bytes, then hash.
std::array<uint8_t, 32> computeDynamicMessageHash(const google::protobuf::Message& message) {
    std::string serialized;
    google::protobuf::io::StringOutputStream stream(&serialized);
    google::protobuf::io::CodedOutputStream coded(&stream);
    coded.SetSerializationDeterministic(true);
    message.SerializeToCodedStream(&coded);
    coded.Trim();

    std::array<uint8_t, 32> hash{};
    SHA256(reinterpret_cast<const uint8_t*>(serialized.data()),
           serialized.size(), hash.data());
    return hash;
}

// ---------------------------------------------------------------------------
// Test: RS sends protobuf → RM transcodes → client receives JSON
//       client sends JSON → RM transcodes → RS receives protobuf
// ---------------------------------------------------------------------------
void testTranscoding() {
    // --- 1. Define an RS-specific .proto ---
    const std::string protoContent = R"(
syntax = "proto3";
package example_rs;

message SensorReading {
    string sensor_id   = 1;
    double temperature = 2;
    uint64 timestamp   = 3;
    bytes  raw_payload = 4;
}

message SensorCommand {
    string sensor_id = 1;
    string command    = 2;
    uint32 priority   = 3;
}
)";

    const std::string protoDir  = ".";
    const std::string protoFile = "transcoding_test_sensor.proto";
    writeFile(protoDir + "/" + protoFile, protoContent);

    // --- 2. RM loads the .proto at runtime ---
    google::protobuf::compiler::DiskSourceTree sourceTree;
    sourceTree.MapPath("", protoDir);
    SilentErrorCollector errorCollector;
    google::protobuf::compiler::Importer importer(&sourceTree, &errorCollector);

    const google::protobuf::FileDescriptor* fileDesc = importer.Import(protoFile);
    require(fileDesc != nullptr, "RM should load RS .proto at runtime");
    require(!errorCollector.hasErrors(), "proto loading should produce no errors");
    if (errorCollector.hasErrors()) errorCollector.printErrors();
    if (fileDesc == nullptr) {
        std::cerr << "  skipping transcoding tests (load failed)\n";
        std::remove(protoFile.c_str());
        return;
    }

    const google::protobuf::Descriptor* sensorReadingDesc =
        fileDesc->FindMessageTypeByName("SensorReading");
    const google::protobuf::Descriptor* sensorCommandDesc =
        fileDesc->FindMessageTypeByName("SensorCommand");
    require(sensorReadingDesc != nullptr, "SensorReading should be found");
    require(sensorCommandDesc != nullptr, "SensorCommand should be found");

    google::protobuf::DynamicMessageFactory factory;

    // --- 3. RS creates a SensorReading and serializes to protobuf bytes ---
    const google::protobuf::Message* readingProto = factory.GetPrototype(sensorReadingDesc);
    std::unique_ptr<google::protobuf::Message> rsReading(readingProto->New());
    {
        const auto* refl = rsReading->GetReflection();
        refl->SetString(rsReading.get(),
                        sensorReadingDesc->FindFieldByName("sensor_id"), "thermo-42");
        refl->SetDouble(rsReading.get(),
                        sensorReadingDesc->FindFieldByName("temperature"), 36.6);
        refl->SetUInt64(rsReading.get(),
                        sensorReadingDesc->FindFieldByName("timestamp"), 1700000000000ULL);
        refl->SetString(rsReading.get(),
                        sensorReadingDesc->FindFieldByName("raw_payload"),
                        std::string("\xDE\xAD\xBE\xEF", 4));
    }

    std::string protobufBytes;
    require(rsReading->SerializeToString(&protobufBytes),
            "RS should serialize SensorReading to protobuf bytes");
    require(!protobufBytes.empty(), "protobuf bytes should not be empty");

    // --- 4. RM receives protobuf bytes, deserializes using dynamic descriptor ---
    std::unique_ptr<google::protobuf::Message> rmReading(readingProto->New());
    require(rmReading->ParseFromString(protobufBytes),
            "RM should deserialize protobuf bytes into dynamic message");

    // Verify the deserialized fields match
    {
        const auto* refl = rmReading->GetReflection();
        require(refl->GetString(*rmReading,
                    sensorReadingDesc->FindFieldByName("sensor_id")) == "thermo-42",
                "RM deserialized sensor_id should match");
        require(refl->GetDouble(*rmReading,
                    sensorReadingDesc->FindFieldByName("temperature")) == 36.6,
                "RM deserialized temperature should match");
        require(refl->GetUInt64(*rmReading,
                    sensorReadingDesc->FindFieldByName("timestamp")) == 1700000000000ULL,
                "RM deserialized timestamp should match");
        require(refl->GetString(*rmReading,
                    sensorReadingDesc->FindFieldByName("raw_payload")) == std::string("\xDE\xAD\xBE\xEF", 4),
                "RM deserialized raw_payload should match");
    }

    // --- 5. RM transcodes to JSON for the JSON-speaking client ---
    std::string jsonOutput;
    google::protobuf::util::JsonPrintOptions printOpts;
    printOpts.always_print_fields_with_no_presence = true;
    absl::Status toJsonStatus =
        google::protobuf::util::MessageToJsonString(*rmReading, &jsonOutput, printOpts);
    require(toJsonStatus.ok(), "RM should convert dynamic message to JSON");
    require(!jsonOutput.empty(), "JSON output should not be empty");

    // Verify the JSON contains expected fields
    require(jsonOutput.find("thermo-42") != std::string::npos,
            "JSON should contain sensor_id value");
    require(jsonOutput.find("36.6") != std::string::npos,
            "JSON should contain temperature value");
    require(jsonOutput.find("1700000000000") != std::string::npos,
            "JSON should contain timestamp value");
    // raw_payload is bytes → protobuf JSON encoding uses base64
    require(jsonOutput.find("rawPayload") != std::string::npos,
            "JSON should contain rawPayload field (camelCase)");

    std::cerr << "  [protobuf->json] " << jsonOutput << "\n";

    // --- 6. Client sends JSON back → RM transcodes to protobuf for RS ---
    //    Simulate a SensorCommand from the JSON-speaking client.
    const std::string clientJson = R"({"sensorId":"thermo-42","command":"calibrate","priority":5})";
    const google::protobuf::Message* commandProto = factory.GetPrototype(sensorCommandDesc);
    std::unique_ptr<google::protobuf::Message> rmCommand(commandProto->New());
    absl::Status fromJsonStatus =
        google::protobuf::util::JsonStringToMessage(clientJson, rmCommand.get());
    require(fromJsonStatus.ok(),
            "RM should parse client JSON into dynamic SensorCommand: " +
            std::string(fromJsonStatus.message()));

    // Verify fields
    {
        const auto* refl = rmCommand->GetReflection();
        require(refl->GetString(*rmCommand,
                    sensorCommandDesc->FindFieldByName("sensor_id")) == "thermo-42",
                "parsed sensor_id should match");
        require(refl->GetString(*rmCommand,
                    sensorCommandDesc->FindFieldByName("command")) == "calibrate",
                "parsed command should match");
        require(refl->GetUInt32(*rmCommand,
                    sensorCommandDesc->FindFieldByName("priority")) == 5,
                "parsed priority should match");
    }

    // RM re-serializes to protobuf for the RS
    std::string commandProtobufBytes;
    require(rmCommand->SerializeToString(&commandProtobufBytes),
            "RM should serialize SensorCommand to protobuf for RS");
    require(!commandProtobufBytes.empty(),
            "serialized SensorCommand bytes should not be empty");

    // RS deserializes and verifies
    std::unique_ptr<google::protobuf::Message> rsCommand(commandProto->New());
    require(rsCommand->ParseFromString(commandProtobufBytes),
            "RS should deserialize the protobuf bytes");
    {
        const auto* refl = rsCommand->GetReflection();
        require(refl->GetString(*rsCommand,
                    sensorCommandDesc->FindFieldByName("sensor_id")) == "thermo-42",
                "RS received sensor_id should match");
        require(refl->GetString(*rsCommand,
                    sensorCommandDesc->FindFieldByName("command")) == "calibrate",
                "RS received command should match");
        require(refl->GetUInt32(*rsCommand,
                    sensorCommandDesc->FindFieldByName("priority")) == 5,
                "RS received priority should match");
    }

    // --- 7. Full round-trip: protobuf → JSON → protobuf ---
    //    Take the original RS SensorReading protobuf bytes, go through
    //    protobuf → JSON → protobuf and confirm the bytes are identical.
    std::string roundTripJson;
    {
        std::unique_ptr<google::protobuf::Message> step1(readingProto->New());
        require(step1->ParseFromString(protobufBytes), "round-trip parse from protobuf");
        absl::Status s = google::protobuf::util::MessageToJsonString(*step1, &roundTripJson);
        require(s.ok(), "round-trip to JSON");
    }
    {
        std::unique_ptr<google::protobuf::Message> step2(readingProto->New());
        absl::Status s = google::protobuf::util::JsonStringToMessage(roundTripJson, step2.get());
        require(s.ok(), "round-trip from JSON");
        std::string roundTripBytes;
        require(step2->SerializeToString(&roundTripBytes), "round-trip serialize");
        require(roundTripBytes == protobufBytes,
                "round-trip protobuf bytes should be identical");
    }

    // --- 8. RM computes a signature hash on the dynamic message ---
    //    The RM hashes the deterministic serialization so it can sign
    //    messages whose schema was loaded at runtime.
    auto hash1 = computeDynamicMessageHash(*rmReading);
    auto hash2 = computeDynamicMessageHash(*rmReading);
    require(hash1 == hash2, "same message should produce the same hash");

    // Modify a field and confirm the hash changes
    {
        auto* refl = rmReading->GetReflection();
        refl->SetDouble(rmReading.get(),
                        sensorReadingDesc->FindFieldByName("temperature"), 99.9);
    }
    auto hash3 = computeDynamicMessageHash(*rmReading);
    require(hash1 != hash3, "modified message should produce a different hash");

    // --- 9. JSON round-trip preserves hash ---
    //    Decode the original bytes, convert to JSON, back to protobuf,
    //    and the hash of the result should match the hash of the original.
    {
        std::unique_ptr<google::protobuf::Message> a(readingProto->New());
        a->ParseFromString(protobufBytes);
        std::string j;
        google::protobuf::util::MessageToJsonString(*a, &j);
        std::unique_ptr<google::protobuf::Message> b(readingProto->New());
        google::protobuf::util::JsonStringToMessage(j, b.get());
        require(computeDynamicMessageHash(*a) == computeDynamicMessageHash(*b),
                "hash should survive JSON round-trip");
    }

    std::remove(protoFile.c_str());
}

void testRSPEncryptedFieldContainerTranscoding() {
    rsp::proto::RSPMessage outbound;
    outbound.mutable_source()->set_value(std::string(16, '\x01'));
    outbound.mutable_nonce()->set_value(std::string(16, '\x02'));

    rsp::proto::FieldEncryptionFixture fixture;
    fixture.set_clear_text("visible");
    fixture.set_secret_text("placeholder");
    fixture.set_secret_bytes(std::string("\xAA\xBB", 2));
    fixture.set_note("test-message");
    rsp::packServiceMessage(outbound, fixture);

    auto* encryptedField = outbound.add_encrypted_fields();
    encryptedField->mutable_path()->add_segments("service_message");
    encryptedField->mutable_path()->add_segments("secret_text");
    encryptedField->set_iv("123456789012");
    encryptedField->set_ciphertext("ciphertext-payload");
    encryptedField->set_tag("0123456789ABCDEF");
    encryptedField->set_algorithm(1);

    std::string protobufBytes;
    require(outbound.SerializeToString(&protobufBytes),
            "RSPMessage with encrypted_fields should serialize to protobuf");

    rsp::proto::RSPMessage decodedFromProtobuf;
    require(decodedFromProtobuf.ParseFromString(protobufBytes),
            "RSPMessage with encrypted_fields should parse from protobuf bytes");
    require(decodedFromProtobuf.encrypted_fields_size() == 1,
            "RSPMessage protobuf decode should keep encrypted_fields");
    require(decodedFromProtobuf.encrypted_fields(0).path().segments_size() == 2,
            "encrypted_fields path should preserve all segments after protobuf decode");

    std::string jsonPayload;
    google::protobuf::util::JsonPrintOptions printOptions;
    printOptions.preserve_proto_field_names = true;
    const auto toJsonStatus =
        google::protobuf::util::MessageToJsonString(decodedFromProtobuf, &jsonPayload, printOptions);
    require(toJsonStatus.ok(),
            "RSPMessage with encrypted_fields should convert to JSON");
    require(jsonPayload.find("encrypted_fields") != std::string::npos,
            "JSON output should include encrypted_fields container");
    require(jsonPayload.find("ciphertext") != std::string::npos,
            "JSON output should include encrypted field payload members");

    rsp::proto::RSPMessage decodedFromJson;
    const auto fromJsonStatus =
        google::protobuf::util::JsonStringToMessage(jsonPayload, &decodedFromJson);
    require(fromJsonStatus.ok(),
            "RSPMessage with encrypted_fields should parse from JSON");
    require(decodedFromJson.encrypted_fields_size() == 1,
            "JSON decode should preserve encrypted_fields entries");
    require(decodedFromJson.encrypted_fields(0).path().segments(1) == "secret_text",
            "JSON decode should preserve encrypted field path values");
    require(decodedFromJson.encrypted_fields(0).ciphertext() == "ciphertext-payload",
            "JSON decode should preserve encrypted field ciphertext bytes");
    require(decodedFromJson.encrypted_fields(0).algorithm() == 1,
            "JSON decode should preserve encrypted field algorithm value");
}

}  // namespace

int main() {
    std::cout << "transcoding_test: RS(protobuf) <-> RM(runtime .proto) <-> Client(JSON) ...\n";

    testTranscoding();
    testRSPEncryptedFieldContainerTranscoding();

    std::cout << "transcoding_test: " << testsRun << " checks, "
              << (testsPassed ? "all passed" : "SOME FAILED") << "\n";
    return testsPassed ? 0 : 1;
}

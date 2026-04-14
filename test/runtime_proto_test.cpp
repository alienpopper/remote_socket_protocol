#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include <cstdio>
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

    void RecordWarning(absl::string_view filename, int line, int column, absl::string_view message) override {
        (void)filename;
        (void)line;
        (void)column;
        (void)message;
    }

    bool hasErrors() const { return !errors_.empty(); }

    void printErrors() const {
        for (const auto& error : errors_) {
            std::cerr << "  proto error: " << error << "\n";
        }
    }

private:
    std::vector<std::string> errors_;
};

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void testLoadRuntimeProto() {
    // Write a simple .proto file to a temp location next to the test binary
    const std::string protoContent =
        "syntax = \"proto3\";\n"
        "package test.dynamic;\n"
        "\n"
        "message Person {\n"
        "  string name = 1;\n"
        "  int32 age = 2;\n"
        "  repeated string tags = 3;\n"
        "}\n"
        "\n"
        "message AddressBook {\n"
        "  repeated Person people = 1;\n"
        "}\n";

    const std::string protoDir = ".";
    const std::string protoFile = "runtime_test_person.proto";
    writeFile(protoDir + "/" + protoFile, protoContent);

    // Set up the source tree and importer
    google::protobuf::compiler::DiskSourceTree sourceTree;
    sourceTree.MapPath("", protoDir);

    SilentErrorCollector errorCollector;
    google::protobuf::compiler::Importer importer(&sourceTree, &errorCollector);

    // Import the proto file at runtime
    const google::protobuf::FileDescriptor* fileDescriptor = importer.Import(protoFile);
    require(fileDescriptor != nullptr, "runtime proto file should load successfully");
    if (errorCollector.hasErrors()) {
        errorCollector.printErrors();
    }
    require(!errorCollector.hasErrors(), "runtime proto loading should not produce errors");

    if (fileDescriptor == nullptr) {
        std::cerr << "  skipping remaining runtime proto tests (load failed)\n";
        return;
    }

    // Verify the file descriptor
    require(std::string(fileDescriptor->name()) == protoFile, "file descriptor name should match proto file");
    require(fileDescriptor->message_type_count() == 2, "proto should define two message types");

    // Find the Person descriptor
    const google::protobuf::Descriptor* personDescriptor =
        fileDescriptor->FindMessageTypeByName("Person");
    require(personDescriptor != nullptr, "Person message should exist in the loaded proto");

    if (personDescriptor == nullptr) {
        std::cerr << "  skipping dynamic message tests (Person not found)\n";
        return;
    }

    require(personDescriptor->field_count() == 3, "Person should have 3 fields");

    const google::protobuf::FieldDescriptor* nameField = personDescriptor->FindFieldByName("name");
    const google::protobuf::FieldDescriptor* ageField = personDescriptor->FindFieldByName("age");
    const google::protobuf::FieldDescriptor* tagsField = personDescriptor->FindFieldByName("tags");
    require(nameField != nullptr, "Person should have a name field");
    require(ageField != nullptr, "Person should have an age field");
    require(tagsField != nullptr, "Person should have a tags field");
    require(nameField != nullptr && nameField->type() == google::protobuf::FieldDescriptor::TYPE_STRING,
            "name field should be string type");
    require(ageField != nullptr && ageField->type() == google::protobuf::FieldDescriptor::TYPE_INT32,
            "age field should be int32 type");
    require(tagsField != nullptr && tagsField->is_repeated(),
            "tags field should be repeated");

    // Create a dynamic message and set fields
    google::protobuf::DynamicMessageFactory factory;
    const google::protobuf::Message* prototype = factory.GetPrototype(personDescriptor);
    require(prototype != nullptr, "dynamic message factory should produce a prototype");

    std::unique_ptr<google::protobuf::Message> person(prototype->New());
    require(person != nullptr, "should create a new dynamic message instance");

    const google::protobuf::Reflection* reflection = person->GetReflection();
    require(reflection != nullptr, "dynamic message should have reflection");

    // Set field values
    reflection->SetString(person.get(), nameField, "Alice");
    reflection->SetInt32(person.get(), ageField, 30);
    reflection->AddString(person.get(), tagsField, "engineer");
    reflection->AddString(person.get(), tagsField, "musician");

    // Verify field values
    require(reflection->GetString(*person, nameField) == "Alice", "name should be Alice");
    require(reflection->GetInt32(*person, ageField) == 30, "age should be 30");
    require(reflection->FieldSize(*person, tagsField) == 2, "tags should have 2 entries");
    require(reflection->GetRepeatedString(*person, tagsField, 0) == "engineer",
            "first tag should be engineer");
    require(reflection->GetRepeatedString(*person, tagsField, 1) == "musician",
            "second tag should be musician");

    // Serialize to bytes and deserialize into a new message
    std::string serializedBytes;
    require(person->SerializeToString(&serializedBytes), "dynamic message should serialize to bytes");
    require(!serializedBytes.empty(), "serialized bytes should not be empty");

    std::unique_ptr<google::protobuf::Message> deserialized(prototype->New());
    require(deserialized->ParseFromString(serializedBytes),
            "dynamic message should deserialize from bytes");

    const google::protobuf::Reflection* deserializedReflection = deserialized->GetReflection();
    require(deserializedReflection->GetString(*deserialized, nameField) == "Alice",
            "deserialized name should be Alice");
    require(deserializedReflection->GetInt32(*deserialized, ageField) == 30,
            "deserialized age should be 30");
    require(deserializedReflection->FieldSize(*deserialized, tagsField) == 2,
            "deserialized tags should have 2 entries");

    // Verify text format output
    std::string textOutput;
    google::protobuf::TextFormat::PrintToString(*person, &textOutput);
    require(textOutput.find("Alice") != std::string::npos,
            "text format output should contain Alice");
    require(textOutput.find("30") != std::string::npos,
            "text format output should contain 30");

    // Test the AddressBook message with nested Person
    const google::protobuf::Descriptor* addressBookDescriptor =
        fileDescriptor->FindMessageTypeByName("AddressBook");
    require(addressBookDescriptor != nullptr, "AddressBook message should exist");

    if (addressBookDescriptor != nullptr) {
        const google::protobuf::FieldDescriptor* peopleField =
            addressBookDescriptor->FindFieldByName("people");
        require(peopleField != nullptr, "AddressBook should have a people field");
        require(peopleField != nullptr && peopleField->is_repeated(),
                "people field should be repeated");
        require(peopleField != nullptr &&
                    peopleField->message_type() == personDescriptor,
                "people field should contain Person messages");

        const google::protobuf::Message* addressBookPrototype =
            factory.GetPrototype(addressBookDescriptor);
        std::unique_ptr<google::protobuf::Message> addressBook(addressBookPrototype->New());
        const google::protobuf::Reflection* abReflection = addressBook->GetReflection();

        // Add a person to the address book
        google::protobuf::Message* addedPerson = abReflection->AddMessage(addressBook.get(), peopleField);
        const google::protobuf::Reflection* addedReflection = addedPerson->GetReflection();
        addedReflection->SetString(addedPerson, nameField, "Bob");
        addedReflection->SetInt32(addedPerson, ageField, 25);

        require(abReflection->FieldSize(*addressBook, peopleField) == 1,
                "address book should have 1 person");

        // Serialize and deserialize the address book
        std::string abBytes;
        require(addressBook->SerializeToString(&abBytes),
                "address book should serialize");
        std::unique_ptr<google::protobuf::Message> abDeserialized(addressBookPrototype->New());
        require(abDeserialized->ParseFromString(abBytes),
                "address book should deserialize");
        require(abDeserialized->GetReflection()->FieldSize(*abDeserialized, peopleField) == 1,
                "deserialized address book should have 1 person");
    }

    // Clean up the temp proto file
    std::remove(protoFile.c_str());
}

} // namespace

int main() {
    std::cout << "runtime_proto_test: loading .proto files at runtime...\n";

    testLoadRuntimeProto();

    std::cout << "runtime_proto_test: " << testsRun << " checks, "
              << (testsPassed ? "all passed" : "SOME FAILED") << "\n";
    return testsPassed ? 0 : 1;
}

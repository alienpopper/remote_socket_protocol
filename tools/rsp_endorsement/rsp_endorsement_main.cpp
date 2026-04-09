#include "common/base_types.hpp"
#include "common/endorsement/endorsement.hpp"
#include "common/endorsement/well_known_endorsements.h"
#include "common/keypair.hpp"
#include "os/os_fileops.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string privateKeyPath;
    std::string publicKeyPath;
    std::string subject;
    std::string endorsementType;
    std::string valueText;
    std::string valueGuid;
    std::string outputPath;
    double validForSeconds = 86400.0;
};

const std::map<std::string, rsp::GUID>& wellKnownGuids() {
    static const std::map<std::string, rsp::GUID> values = {
        {"access", ETYPE_ACCESS},
        {"network-access", EVALUE_ACCESS_NETWORK},
        {"register-names", EVALUE_REGISTER_NAMES},
        {"role", ETYPE_ROLE},
        {"client", EVALUE_ROLE_CLIENT},
        {"resource-manager", EVALUE_ROLE_RESOURCE_MANAGER},
        {"resource-service", EVALUE_ROLE_RESOURCE_SERVICE},
        {"endorsement-service", EVALUE_ROLE_ENDORSEMENT_SERVICE},
        {"name-service", EVALUE_ROLE_NAME_SERVICE},
    };
    return values;
}

void printUsage(const char* programName) {
    std::cerr
        << "Usage:\n"
    << "  " << programName << " --private-key <path> --public-key <path> --subject <node-id>\n"
    << "     --type <guid-or-alias> (--value-text <text> | --value-guid <guid-or-alias>)\n"
    << "     --output <path> [--valid-for-seconds <seconds>]\n\n"
        << "Supported aliases:\n"
    << "  type: access, role\n"
    << "  value-guid: network-access, register-names, client, resource-manager,\n"
    << "              resource-service, endorsement-service, name-service\n";
}

std::string requireValue(int& index, int argc, char* argv[], const std::string& flag) {
    if (index + 1 >= argc) {
        throw std::runtime_error("missing value for " + flag);
    }

    ++index;
    return argv[index];
}

Options parseArguments(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--private-key") {
            options.privateKeyPath = requireValue(index, argc, argv, argument);
        } else if (argument == "--public-key") {
            options.publicKeyPath = requireValue(index, argc, argv, argument);
        } else if (argument == "--subject") {
            options.subject = requireValue(index, argc, argv, argument);
        } else if (argument == "--type") {
            options.endorsementType = requireValue(index, argc, argv, argument);
        } else if (argument == "--value-text") {
            options.valueText = requireValue(index, argc, argv, argument);
        } else if (argument == "--value-guid") {
            options.valueGuid = requireValue(index, argc, argv, argument);
        } else if (argument == "--output") {
            options.outputPath = requireValue(index, argc, argv, argument);
        } else if (argument == "--valid-for-seconds") {
            options.validForSeconds = std::stod(requireValue(index, argc, argv, argument));
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }

    if (options.privateKeyPath.empty() || options.publicKeyPath.empty() || options.subject.empty() ||
        options.endorsementType.empty() || options.outputPath.empty()) {
        throw std::runtime_error("missing required arguments");
    }

    if (options.valueText.empty() == options.valueGuid.empty()) {
        throw std::runtime_error("provide exactly one of --value-text or --value-guid");
    }

    if (options.validForSeconds <= 0.0) {
        throw std::runtime_error("--valid-for-seconds must be greater than zero");
    }

    return options;
}

rsp::GUID parseGuidOrAlias(const std::string& input) {
    const auto known = wellKnownGuids().find(input);
    if (known != wellKnownGuids().end()) {
        return known->second;
    }

    return rsp::GUID(input);
}

rsp::Buffer guidToBuffer(const rsp::GUID& guid) {
    std::string bytes;
    bytes.reserve(16);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((guid.high() >> shift) & 0xFFULL));
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((guid.low() >> shift) & 0xFFULL));
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()));
}

rsp::Buffer textToBuffer(const std::string& value) {
    if (value.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

void writeBufferToDisk(const std::string& path, const rsp::Buffer& buffer) {
    std::FILE* file = rsp::os::openFile(path, "wb");
    if (file == nullptr) {
        throw std::runtime_error("failed to open output path for writing: " + path);
    }

    const size_t bytesWritten = buffer.empty()
        ? 0
        : std::fwrite(buffer.data(), 1, static_cast<size_t>(buffer.size()), file);
    const int closeResult = std::fclose(file);

    if (bytesWritten != static_cast<size_t>(buffer.size()) || closeResult != 0) {
        throw std::runtime_error("failed to write endorsement to disk: " + path);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parseArguments(argc, argv);
        const rsp::KeyPair issuerKeyPair = rsp::KeyPair::readFromDisk(options.privateKeyPath, options.publicKeyPath);
        const rsp::NodeID subject(options.subject);
        const rsp::GUID endorsementType = parseGuidOrAlias(options.endorsementType);
        const rsp::Buffer endorsementValue = options.valueGuid.empty()
            ? textToBuffer(options.valueText)
            : guidToBuffer(parseGuidOrAlias(options.valueGuid));

        rsp::DateTime validUntil;
        validUntil += options.validForSeconds;

        const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
            issuerKeyPair,
            subject,
            endorsementType,
            endorsementValue,
            validUntil);
        const rsp::Buffer serialized = endorsement.serialize();
        writeBufferToDisk(options.outputPath, serialized);

        std::cout << "created endorsement\n";
        std::cout << "  subject: " << endorsement.subject().toString() << "\n";
        std::cout << "  issuer: " << endorsement.endorsementService().toString() << "\n";
        std::cout << "  type: " << endorsement.endorsementType().toString() << "\n";
        std::cout << "  valid_until_ms: " << endorsement.validUntil().millisecondsSinceEpoch() << "\n";
        std::cout << "  output: " << options.outputPath << "\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "rsp_endorsement failed: " << exception.what() << "\n";
        printUsage(argv[0]);
        return 1;
    }
}
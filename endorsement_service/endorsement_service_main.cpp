#include "endorsement_service/endorsement_service.hpp"

#include "common/keypair.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include <exception>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

static nlohmann::json loadConfigFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open config file: " + path);
    }
    nlohmann::json j;
    file >> j;
    return j;
}

static nlohmann::json mergeConfigs(const std::vector<std::string>& paths) {
    nlohmann::json merged = nlohmann::json::object();
    for (const auto& path : paths) {
        auto j = loadConfigFile(path);
        for (auto it = j.begin(); it != j.end(); ++it) {
            merged[it.key()] = it.value();
        }
    }
    return merged;
}

static rsp::Buffer stringToBuffer(const std::string& value) {
    if (value.empty()) {
        return rsp::Buffer();
    }
    return rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

static std::optional<rsp::NodeID> parseConfiguredRequestor(const nlohmann::json& entry) {
    const char* requestorField = nullptr;
    if (entry.contains("requestor")) {
        requestorField = "requestor";
    } else if (entry.contains("subject")) {
        requestorField = "subject";
    }

    if (requestorField == nullptr) {
        return std::nullopt;
    }

    if (!entry[requestorField].is_string()) {
        throw std::runtime_error(std::string("\"endorsements[].") + requestorField + "\" must be a string");
    }

    const std::string requestor = entry[requestorField].get<std::string>();
    if (requestor == "*" || requestor == "any" || requestor == "ANY") {
        return std::nullopt;
    }

    return rsp::NodeID(requestor);
}

static double parseConfiguredValiditySeconds(const nlohmann::json& entry) {
    if (entry.contains("valid_for_seconds")) {
        if (!entry["valid_for_seconds"].is_number()) {
            throw std::runtime_error("\"endorsements[].valid_for_seconds\" must be numeric");
        }
        const double value = entry["valid_for_seconds"].get<double>();
        if (value <= 0.0) {
            throw std::runtime_error("\"endorsements[].valid_for_seconds\" must be greater than zero");
        }
        return value;
    }

    if (entry.contains("valid_hours")) {
        if (!entry["valid_hours"].is_number()) {
            throw std::runtime_error("\"endorsements[].valid_hours\" must be numeric");
        }
        const double value = entry["valid_hours"].get<double>();
        if (value <= 0.0) {
            throw std::runtime_error("\"endorsements[].valid_hours\" must be greater than zero");
        }
        return HOURS(value);
    }

    if (entry.contains("valid_days")) {
        if (!entry["valid_days"].is_number()) {
            throw std::runtime_error("\"endorsements[].valid_days\" must be numeric");
        }
        const double value = entry["valid_days"].get<double>();
        if (value <= 0.0) {
            throw std::runtime_error("\"endorsements[].valid_days\" must be greater than zero");
        }
        return DAYS(value);
    }

    return DAYS(1);
}

static std::vector<rsp::endorsement_service::EndorsementService::ConfiguredEndorsement>
parseConfiguredEndorsements(const nlohmann::json& config) {
    std::vector<rsp::endorsement_service::EndorsementService::ConfiguredEndorsement> endorsements;

    if (!config.contains("endorsements")) {
        return endorsements;
    }

    if (!config["endorsements"].is_array()) {
        throw std::runtime_error("\"endorsements\" must be an array");
    }

    for (const auto& entry : config["endorsements"]) {
        if (!entry.is_object()) {
            throw std::runtime_error("\"endorsements\" entries must be objects");
        }

        if (!entry.contains("endorsement_type") || !entry["endorsement_type"].is_string()) {
            throw std::runtime_error("\"endorsements[].endorsement_type\" must be a GUID string");
        }

        std::string endorsementValue;
        if (entry.contains("endorsement_value")) {
            if (!entry["endorsement_value"].is_string()) {
                throw std::runtime_error("\"endorsements[].endorsement_value\" must be a string");
            }
            endorsementValue = entry["endorsement_value"].get<std::string>();
        }

        rsp::endorsement_service::EndorsementService::ConfiguredEndorsement configuredEndorsement;
        configuredEndorsement.requestor = parseConfiguredRequestor(entry);
        configuredEndorsement.endorsementType = rsp::GUID(entry["endorsement_type"].get<std::string>());
        configuredEndorsement.endorsementValue = stringToBuffer(endorsementValue);
        configuredEndorsement.validForSeconds = parseConfiguredValiditySeconds(entry);
        endorsements.push_back(std::move(configuredEndorsement));
    }

    return endorsements;
}

static void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [--config <file>]...\n"
              << "  --config <file>  Load a JSON configuration file (may be repeated)\n";
}

int main(int argc, char** argv) {
    std::vector<std::string> configPaths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" || arg == "-c") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a file argument\n";
                printUsage(argv[0]);
                return 1;
            }
            configPaths.push_back(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "error: unknown option: " << arg << '\n';
            printUsage(argv[0]);
            return 1;
        }
    }

    if (configPaths.empty()) {
        std::cerr << "error: at least one --config file is required\n";
        printUsage(argv[0]);
        return 1;
    }

    nlohmann::json config;
    try {
        config = mergeConfigs(configPaths);
    } catch (const std::exception& e) {
        std::cerr << "error loading config: " << e.what() << '\n';
        return 1;
    }

    std::string rmTransportSpec;
    if (config.contains("rm_servers") && !config["rm_servers"].empty()) {
        const std::string addr = config["rm_servers"][0].get<std::string>();
        rmTransportSpec = "tcp:" + addr;
    } else {
        std::cerr << "error: no rm_servers configured\n";
        return 1;
    }

    rsp::endorsement_service::EndorsementService::Ptr endorsementService;
    if (config.contains("key_file")) {
        try {
            auto keyPair = rsp::KeyPair::loadOrGenerate(config["key_file"].get<std::string>());
            endorsementService = rsp::endorsement_service::EndorsementService::create(std::move(keyPair));
        } catch (const std::exception& e) {
            std::cerr << "error with key_file: " << e.what() << '\n';
            return 1;
        }
    } else if (config.contains("keypair")) {
        const auto& kpArray = config["keypair"];
        if (!kpArray.is_array() || kpArray.size() != 2) {
            std::cerr << "error: \"keypair\" must be an array of [\"<public_key_path>\", \"<private_key_path>\"]\n";
            return 1;
        }
        const std::string publicKeyPath = kpArray[0].get<std::string>();
        const std::string privateKeyPath = kpArray[1].get<std::string>();
        try {
            auto keyPair = rsp::KeyPair::readFromDisk(privateKeyPath, publicKeyPath);
            endorsementService = rsp::endorsement_service::EndorsementService::create(std::move(keyPair));
        } catch (const std::exception& e) {
            std::cerr << "error loading keypair: " << e.what() << '\n';
            return 1;
        }
    } else {
        endorsementService = rsp::endorsement_service::EndorsementService::create();
    }

    try {
        endorsementService->setConfiguredEndorsements(parseConfiguredEndorsements(config));
    } catch (const std::exception& e) {
        std::cerr << "error parsing endorsements config: " << e.what() << '\n';
        return 1;
    }

    try {
        endorsementService->connectToResourceManager(rmTransportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        std::cout << "rsp_es: connected to " << rmTransportSpec
                  << " using encoding " << rsp::message_queue::kAsciiHandshakeEncoding << '\n';
        std::cout << "endorsement service node ID: " << endorsementService->nodeId().toString() << '\n';
        std::cout << "configured endorsements: " << endorsementService->configuredEndorsementCount() << '\n';
        std::cout.flush();
        return endorsementService->run();
    } catch (const std::exception& exception) {
        std::cerr << "endorsement_service failed: " << exception.what() << '\n';
        return 1;
    }
}

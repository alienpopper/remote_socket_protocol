#include "name_service/name_service.hpp"

#include "common/keypair.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include <exception>
#include <fstream>
#include <iostream>
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

    // Determine the resource manager address.
    std::string rmTransportSpec;
    if (config.contains("rm_servers") && !config["rm_servers"].empty()) {
        const std::string addr = config["rm_servers"][0].get<std::string>();
        rmTransportSpec = "tcp:" + addr;
    } else {
        std::cerr << "error: no rm_servers configured\n";
        return 1;
    }

    // Load keypair from config or generate a new one.
    rsp::name_service::NameService::Ptr nameService;
    if (config.contains("keypair")) {
        const auto& kpArray = config["keypair"];
        if (!kpArray.is_array() || kpArray.size() != 2) {
            std::cerr << "error: \"keypair\" must be an array of [\"<public_key_path>\", \"<private_key_path>\"]\n";
            return 1;
        }
        const std::string publicKeyPath = kpArray[0].get<std::string>();
        const std::string privateKeyPath = kpArray[1].get<std::string>();
        try {
            auto keyPair = rsp::KeyPair::readFromDisk(privateKeyPath, publicKeyPath);
            nameService = rsp::name_service::NameService::create(std::move(keyPair));
        } catch (const std::exception& e) {
            std::cerr << "error loading keypair: " << e.what() << '\n';
            return 1;
        }
    } else {
        nameService = rsp::name_service::NameService::create();
    }

    try {
        nameService->connectToResourceManager(rmTransportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        std::cout << "rsp_name_service: connected to " << rmTransportSpec
                  << " using encoding " << rsp::message_queue::kAsciiHandshakeEncoding << '\n';
        return nameService->run();
    } catch (const std::exception& exception) {
        std::cerr << "rsp_name_service failed: " << exception.what() << '\n';
        return 1;
    }
}

#include "resource_manager/resource_manager.hpp"

#include "common/endorsement/endorsement_text.hpp"
#include "common/keypair.hpp"
#include "common/transport/transport_tcp.hpp"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include <condition_variable>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static std::mutex gShutdownMutex;
static std::condition_variable gShutdownCv;
static bool gShouldShutdown = false;

static void signalHandler(int) {
    std::lock_guard<std::mutex> lock(gShutdownMutex);
    gShouldShutdown = true;
    gShutdownCv.notify_all();
}

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

static rsp::proto::ERDAbstractSyntaxTree buildMessageRulesTree(const nlohmann::json& rules) {
    if (rules.empty()) {
        rsp::proto::ERDAbstractSyntaxTree tree;
        tree.mutable_true_value();
        return tree;
    }

    if (rules.size() == 1) {
        return rsp::erd_text::fromString(rules[0].get<std::string>());
    }

    rsp::proto::ERDAbstractSyntaxTree combined = rsp::erd_text::fromString(rules[0].get<std::string>());
    for (size_t i = 1; i < rules.size(); ++i) {
        rsp::proto::ERDAbstractSyntaxTree next;
        *next.mutable_and_()->mutable_lhs() = std::move(combined);
        *next.mutable_and_()->mutable_rhs() = rsp::erd_text::fromString(rules[i].get<std::string>());
        combined = std::move(next);
    }
    return combined;
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

    // Set up transports from "transports" array.
    std::vector<rsp::transport::ListeningTransportHandle> transports;
    if (config.contains("transports")) {
        for (const auto& spec : config["transports"]) {
            const std::string transportSpec = spec.get<std::string>();

            // Expected format: "tcp:<address>:<port>"
            const std::string tcpPrefix = "tcp:";
            if (transportSpec.substr(0, tcpPrefix.size()) != tcpPrefix) {
                std::cerr << "error: unsupported transport: " << transportSpec << '\n';
                return 1;
            }

            const std::string endpoint = transportSpec.substr(tcpPrefix.size());
            auto transport = std::make_shared<rsp::transport::TcpTransport>();
            if (!transport->listen(endpoint)) {
                std::cerr << "resource_manager: failed to listen on " << endpoint << '\n';
                return 1;
            }
            std::cout << "resource_manager: listening on " << endpoint << std::endl;
            transports.push_back(transport);
        }
    }

    if (transports.empty()) {
        std::cerr << "error: no transports configured\n";
        return 1;
    }

    // Build authorization tree from "message_rules" array.
    rsp::proto::ERDAbstractSyntaxTree authzTree;
    if (config.contains("message_rules")) {
        try {
            authzTree = buildMessageRulesTree(config["message_rules"]);
        } catch (const std::exception& e) {
            std::cerr << "error parsing message_rules: " << e.what() << '\n';
            return 1;
        }
    } else {
        authzTree.mutable_true_value();
    }

    // Load keypair from config or generate a new one.
    bool hasKeypair = false;
    rsp::KeyPair loadedKeyPair;
    if (config.contains("keypair")) {
        const auto& kpArray = config["keypair"];
        if (!kpArray.is_array() || kpArray.size() != 2) {
            std::cerr << "error: \"keypair\" must be an array of [\"<public_key_path>\", \"<private_key_path>\"]\n";
            return 1;
        }
        const std::string publicKeyPath = kpArray[0].get<std::string>();
        const std::string privateKeyPath = kpArray[1].get<std::string>();
        try {
            loadedKeyPair = rsp::KeyPair::readFromDisk(privateKeyPath, publicKeyPath);
            hasKeypair = true;
        } catch (const std::exception& e) {
            std::cerr << "error loading keypair: " << e.what() << '\n';
            return 1;
        }
    }

    class ConfiguredResourceManager : public rsp::resource_manager::ResourceManager {
    public:
        ConfiguredResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports,
                                  rsp::proto::ERDAbstractSyntaxTree tree)
            : ResourceManager(std::move(clientTransports)), authzTree_(std::move(tree)) {
            rebuildAuthorizationQueue();
        }

        ConfiguredResourceManager(rsp::KeyPair keyPair,
                                  std::vector<rsp::transport::ListeningTransportHandle> clientTransports,
                                  rsp::proto::ERDAbstractSyntaxTree tree)
            : ResourceManager(std::move(keyPair), std::move(clientTransports)), authzTree_(std::move(tree)) {
            rebuildAuthorizationQueue();
        }

        rsp::NodeID nodeId() const { return keyPair().nodeID(); }

    protected:
        rsp::proto::ERDAbstractSyntaxTree authorizationTree() const override {
            return authzTree_;
        }

    private:
        rsp::proto::ERDAbstractSyntaxTree authzTree_;
    };

    std::unique_ptr<ConfiguredResourceManager> rm;
    if (hasKeypair) {
        rm = std::make_unique<ConfiguredResourceManager>(
            std::move(loadedKeyPair), std::move(transports), std::move(authzTree));
    } else {
        rm = std::make_unique<ConfiguredResourceManager>(
            std::move(transports), std::move(authzTree));
    }

    std::cout << "resource_manager: node ID " << rm->nodeId().toString() << std::endl;

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    std::unique_lock<std::mutex> lock(gShutdownMutex);
    gShutdownCv.wait(lock, []() { return gShouldShutdown; });

    return 0;
}
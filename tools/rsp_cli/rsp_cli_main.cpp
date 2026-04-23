#include "client/cpp/rsp_client_message.hpp"
#include "common/base_types.hpp"
#include "common/message_queue/mq_ascii_handshake.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

const char* kDefaultTransport = "tcp:127.0.0.1:35000";
const char* kDefaultEncoding = "protobuf";

std::optional<rsp::NodeID> fromProtoNodeId(const rsp::proto::NodeId& nodeId) {
    if (nodeId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, nodeId.value().data(), sizeof(high));
    std::memcpy(&low, nodeId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

std::string formatAddress(const rsp::proto::Address& address) {
    if (address.ipv4() != 0) {
        const uint32_t ip = address.ipv4();
        return std::to_string((ip >> 24) & 0xFF) + "." +
               std::to_string((ip >> 16) & 0xFF) + "." +
               std::to_string((ip >> 8) & 0xFF) + "." +
               std::to_string(ip & 0xFF);
    }

    if (!address.ipv6().empty()) {
        return "(ipv6)";
    }

    return "0.0.0.0";
}

std::string formatNodeId(const rsp::proto::NodeId& protoId) {
    const auto nodeId = fromProtoNodeId(protoId);
    if (!nodeId.has_value()) {
        return "(unknown)";
    }
    return nodeId->toString();
}

void printRecord(const rsp::proto::ResourceRecord& record) {
    if (record.has_tcp_connect()) {
        const auto& tcp = record.tcp_connect();
        std::cout << "  tcp_connect";
        if (tcp.has_node_id()) {
            std::cout << "  node=" << formatNodeId(tcp.node_id());
        }
        if (tcp.source_addresses_size() > 0) {
            std::cout << "  addresses=[";
            for (int i = 0; i < tcp.source_addresses_size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << formatAddress(tcp.source_addresses(i));
            }
            std::cout << "]";
        }
        std::cout << "\n";
    } else if (record.has_tcp_listen()) {
        const auto& tcp = record.tcp_listen();
        std::cout << "  tcp_listen";
        if (tcp.has_node_id()) {
            std::cout << "  node=" << formatNodeId(tcp.node_id());
        }
        if (tcp.listen_address_size() > 0) {
            std::cout << "  addresses=[";
            for (int i = 0; i < tcp.listen_address_size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << formatAddress(tcp.listen_address(i));
            }
            std::cout << "]";
        }
        if (tcp.has_allowed_range()) {
            std::cout << "  ports=" << tcp.allowed_range().start_port()
                      << "-" << tcp.allowed_range().end_port();
        }
        std::cout << "\n";
    } else if (record.has_sshd()) {
        const auto& sshd = record.sshd();
        std::cout << "  sshd";
        if (sshd.has_node_id()) {
            std::cout << "  node=" << formatNodeId(sshd.node_id());
        }
        if (sshd.has_server_name()) {
            std::cout << "  server=" << sshd.server_name();
        }
        std::cout << "\n";
    } else {
        std::cout << "  (unknown record type)\n";
    }
}

void printDiscoveredService(const rsp::proto::DiscoveredService& service) {
    std::cout << "  service";
    if (service.has_node_id()) {
        std::cout << "  node=" << formatNodeId(service.node_id());
    }
    if (service.has_schema()) {
        const auto& schema = service.schema();
        std::cout << "  proto=" << schema.proto_file_name();
        std::cout << "  version=" << schema.schema_version();
        if (schema.accepted_type_urls_size() > 0) {
            std::cout << "  accepts=[";
            for (int index = 0; index < schema.accepted_type_urls_size(); ++index) {
                if (index > 0) std::cout << ", ";
                std::cout << schema.accepted_type_urls(index);
            }
            std::cout << "]";
        }
    }
    std::cout << "\n";
}

int resourceList(const std::string& transport) {
    auto client = rsp::client::RSPClientMessage::create();

    const auto connectionId = client->connectToResourceManager(transport, rsp::message_queue::kAsciiHandshakeEncoding);
    if (!connectionId.has_value()) {
        std::cerr << "error: failed to connect to resource manager\n";
        return 1;
    }

    const auto rmNodeId = client->peerNodeID(*connectionId);
    if (!rmNodeId.has_value()) {
        std::cerr << "error: failed to obtain resource manager node id\n";
        return 1;
    }

    rsp::proto::RSPMessage query;
    *query.mutable_source() = toProtoNodeId(client->nodeId());
    *query.mutable_destination() = toProtoNodeId(*rmNodeId);
    query.mutable_resource_query();

    if (!client->send(query)) {
        std::cerr << "error: failed to send resource query\n";
        return 1;
    }

    rsp::proto::RSPMessage reply;
    constexpr int maxAttempts = 100;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (client->waitAndDequeueMessage(reply)) {
            break;
        }
    }

    if (!reply.has_resource_query_reply()) {
        std::cerr << "error: no resource query reply received from resource manager\n";
        return 1;
    }

    const auto& queryReply = reply.resource_query_reply();
    if (queryReply.services_size() == 0) {
        std::cout << "no services discovered\n";
        return 0;
    }

    std::cout << queryReply.services_size() << " discovered service(s):\n";
    for (const auto& service : queryReply.services()) {
        printDiscoveredService(service);
    }

    return 0;
}

struct GlobalOptions {
    std::string transport = kDefaultTransport;
};

void printUsage(const char* programName) {
    std::cerr
        << "Usage: " << programName << " [--transport <spec>] <command> [arguments]\n\n"
        << "Global options:\n"
        << "  --transport <spec>   Resource manager transport (default: " << kDefaultTransport << ")\n\n"
        << "Commands:\n"
        << "  resource list        List all advertised resources\n";
}

} // namespace

int main(int argc, char* argv[]) {
    GlobalOptions globals;
    int argIndex = 1;

    while (argIndex < argc && argv[argIndex][0] == '-') {
        const std::string flag = argv[argIndex];
        if (flag == "--transport") {
            if (argIndex + 1 >= argc) {
                std::cerr << "error: missing value for --transport\n";
                return 1;
            }
            ++argIndex;
            globals.transport = argv[argIndex];
            ++argIndex;
        } else if (flag == "--help" || flag == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "error: unknown option: " << flag << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (argIndex >= argc) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string command = argv[argIndex];
    ++argIndex;

    if (command == "resource") {
        if (argIndex >= argc) {
            std::cerr << "error: missing subcommand for 'resource'\n";
            std::cerr << "  resource list   List all advertised resources\n";
            return 1;
        }

        const std::string subcommand = argv[argIndex];
        ++argIndex;

        if (subcommand == "list") {
            return resourceList(globals.transport);
        } else {
            std::cerr << "error: unknown resource subcommand: " << subcommand << "\n";
            return 1;
        }
    } else {
        std::cerr << "error: unknown command: " << command << "\n";
        printUsage(argv[0]);
        return 1;
    }
}

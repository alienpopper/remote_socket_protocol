#include "endorsement_service/endorsement_service.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string transportSpec = argc > 1 ? argv[1] : "tcp:127.0.0.1:35000";

    try {
        auto endorsementService = rsp::endorsement_service::EndorsementService::create();
        endorsementService->connectToResourceManager(transportSpec, rsp::message_queue::kAsciiHandshakeEncoding);
        std::cout << "endorsement service connected to " << transportSpec
              << " using encoding " << rsp::message_queue::kAsciiHandshakeEncoding << '\n';
        return endorsementService->run();
    } catch (const std::exception& exception) {
        std::cerr << "endorsement_service failed: " << exception.what() << '\n';
        return 1;
    }
}

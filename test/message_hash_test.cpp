#include "common/message_queue/mq_signing.hpp"
#include "common/service_message.hpp"

#include "logging/logging.pb.h"
#include "resource_service/bsd_sockets/bsd_sockets.pb.h"

#include <iostream>
#include <stdexcept>
#include <string>

using rsp::MessageHash;
using rsp::computeMessageHash;

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------

#define CHECK(expr) \
    do { \
        if (!(expr)) \
            throw std::runtime_error("CHECK failed: " #expr); \
    } while (0)

static int g_passed = 0;
static int g_failed = 0;

static void run(const char* name, void (*fn)()) {
    try {
        fn();
        std::cout << "PASS: " << name << "\n";
        ++g_passed;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << name << ": " << e.what() << "\n";
        ++g_failed;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static rsp::proto::RSPMessage makePingRequest(uint32_t sequence = 1) {
    rsp::proto::RSPMessage msg;
    msg.mutable_source()->set_value(std::string(16, '\x01'));
    auto* pr = msg.mutable_ping_request();
    pr->mutable_nonce()->set_value(std::string(16, '\xAB'));
    pr->set_sequence(sequence);
    return msg;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_hash_is_32_bytes() {
    const auto h = computeMessageHash(makePingRequest());
    CHECK(h.size() == 32);
}

static void test_empty_message_deterministic() {
    rsp::proto::RSPMessage msg;
    const auto h1 = computeMessageHash(msg);
    const auto h2 = computeMessageHash(msg);
    CHECK(h1 == h2);
}

static void test_ping_request_deterministic() {
    const auto h1 = computeMessageHash(makePingRequest());
    const auto h2 = computeMessageHash(makePingRequest());
    CHECK(h1 == h2);
}

static void test_changing_sequence_changes_hash() {
    const auto h1 = computeMessageHash(makePingRequest(1));
    const auto h2 = computeMessageHash(makePingRequest(2));
    CHECK(h1 != h2);
}

static void test_signature_field_excluded() {
    auto msg = makePingRequest();
    const auto h_before = computeMessageHash(msg);

    // Set an arbitrary signature — hash must not change.
    auto* sig = msg.mutable_signature();
    sig->mutable_signer()->set_value(std::string(16, '\xFF'));
    sig->set_algorithm(rsp::proto::P256);
    sig->set_signature("fake-sig");

    const auto h_after = computeMessageHash(msg);
    CHECK(h_before == h_after);
}

static void test_source_field_changes_hash() {
    auto msg1 = makePingRequest();
    auto msg2 = makePingRequest();
    msg2.mutable_source()->set_value(std::string(16, '\x02'));
    CHECK(computeMessageHash(msg1) != computeMessageHash(msg2));
}

static void test_destination_absent_vs_present() {
    auto without_dest = makePingRequest();
    auto with_dest = makePingRequest();
    with_dest.mutable_destination()->set_value(std::string(16, '\x99'));
    CHECK(computeMessageHash(without_dest) != computeMessageHash(with_dest));
}

static void test_different_submessage_types_differ() {
    rsp::proto::RSPMessage ping_req;
    ping_req.mutable_source()->set_value(std::string(16, '\x01'));
    ping_req.mutable_ping_request()->set_sequence(42);

    rsp::proto::RSPMessage ping_rep;
    ping_rep.mutable_source()->set_value(std::string(16, '\x01'));
    ping_rep.mutable_ping_reply()->set_sequence(42);

    CHECK(computeMessageHash(ping_req) != computeMessageHash(ping_rep));
}

static void test_endorsements_affect_hash() {
    auto msg_no_end = makePingRequest();
    auto msg_with_end = makePingRequest();
    auto* end = msg_with_end.add_endorsements();
    end->mutable_subject()->set_value(std::string(16, '\x55'));
    CHECK(computeMessageHash(msg_no_end) != computeMessageHash(msg_with_end));
}

static void test_endorsements_deterministic() {
    auto mk = []() {
        auto msg = makePingRequest();
        auto* end = msg.add_endorsements();
        end->mutable_subject()->set_value(std::string(16, '\x55'));
        end->set_endorsement_value("test");
        return msg;
    };
    CHECK(computeMessageHash(mk()) == computeMessageHash(mk()));
}

static void test_socket_send_field_order() {
    // StreamSend has data=2 and index=3 in non-numeric proto order;
    // verify that changing either affects the hash distinctly.
    auto mk = [](const std::string& data, uint64_t index) {
        rsp::proto::RSPMessage msg;
        msg.mutable_source()->set_value(std::string(16, '\x01'));
        rsp::proto::StreamSend s;
        s.mutable_stream_id()->set_value(std::string(16, '\x01'));
        s.set_data(data);
        s.set_index(index);
        rsp::packServiceMessage(msg, s);
        return msg;
    };
    const auto h_a = computeMessageHash(mk("hello", 1));
    const auto h_b = computeMessageHash(mk("hello", 2));
    const auto h_c = computeMessageHash(mk("world", 1));
    CHECK(h_a != h_b);
    CHECK(h_a != h_c);
    CHECK(h_b != h_c);
    // Determinism
    CHECK(computeMessageHash(mk("hello", 1)) == h_a);
}

static void test_connect_tcp_request() {
    auto mk = [](const std::string& host) {
        rsp::proto::RSPMessage msg;
        msg.mutable_source()->set_value(std::string(16, '\x01'));
        rsp::proto::ConnectTCPRequest c;
        c.set_host_port(host);
        c.mutable_stream_id()->set_value(std::string(16, '\x02'));
        rsp::packServiceMessage(msg, c);
        return msg;
    };
    CHECK(computeMessageHash(mk("127.0.0.1:8080")) !=
          computeMessageHash(mk("127.0.0.1:9090")));
    CHECK(computeMessageHash(mk("127.0.0.1:8080")) ==
          computeMessageHash(mk("127.0.0.1:8080")));
}

static void test_route_update() {
    auto mk = [](uint32_t hops) {
        rsp::proto::RSPMessage msg;
        msg.mutable_source()->set_value(std::string(16, '\x01'));
        auto* entry = msg.mutable_route()->add_entries();
        entry->mutable_node_id()->set_value(std::string(16, '\xBB'));
        entry->set_hops_away(hops);
        return msg;
    };
    CHECK(computeMessageHash(mk(1)) != computeMessageHash(mk(2)));
    CHECK(computeMessageHash(mk(3)) == computeMessageHash(mk(3)));
}

static void test_endorsement_challenge() {
    auto mk = [](uint32_t stage, const std::string& challenge) {
        rsp::proto::RSPMessage msg;
        msg.mutable_source()->set_value(std::string(16, '\x01'));
        rsp::proto::EndorsementChallenge ec;
        ec.set_stage(stage);
        ec.set_challenge(challenge);
        rsp::packServiceMessage(msg, ec);
        return msg;
    };
    CHECK(computeMessageHash(mk(1, "abc")) != computeMessageHash(mk(2, "abc")));
    CHECK(computeMessageHash(mk(1, "abc")) != computeMessageHash(mk(1, "xyz")));
    CHECK(computeMessageHash(mk(1, "abc")) == computeMessageHash(mk(1, "abc")));
}

static void test_logging_message_hash() {
    auto mk = [](const std::string& textMessage) {
        rsp::proto::RSPMessage msg;
        msg.mutable_source()->set_value(std::string(16, '\x01'));

        auto* record = msg.mutable_log_record();

        rsp::proto::LogText text;
        text.set_message(textMessage);
        text.add_tags("hash");
        record->mutable_payload()->PackFrom(text, rsp::kTypeUrlPrefix);
        return msg;
    };

        CHECK(computeMessageHash(mk("one")) != computeMessageHash(mk("two")));
        CHECK(computeMessageHash(mk("one")) == computeMessageHash(mk("one")));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    run("hash_is_32_bytes",               test_hash_is_32_bytes);
    run("empty_message_deterministic",    test_empty_message_deterministic);
    run("ping_request_deterministic",     test_ping_request_deterministic);
    run("changing_sequence_changes_hash", test_changing_sequence_changes_hash);
    run("signature_field_excluded",       test_signature_field_excluded);
    run("source_field_changes_hash",      test_source_field_changes_hash);
    run("destination_absent_vs_present",  test_destination_absent_vs_present);
    run("different_submessage_types",     test_different_submessage_types_differ);
    run("endorsements_affect_hash",       test_endorsements_affect_hash);
    run("endorsements_deterministic",     test_endorsements_deterministic);
    run("socket_send_field_order",        test_socket_send_field_order);
    run("connect_tcp_request",            test_connect_tcp_request);
    run("route_update",                   test_route_update);
    run("endorsement_challenge",          test_endorsement_challenge);
    run("logging_message_hash",           test_logging_message_hash);

    std::cout << "\n" << g_passed << "/" << (g_passed + g_failed) << " tests passed\n";
    return g_failed > 0 ? 1 : 0;
}

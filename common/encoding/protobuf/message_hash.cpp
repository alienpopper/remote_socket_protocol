#include "common/encoding/protobuf/message_hash.hpp"

#include <openssl/sha.h>

#include <cstdint>
#include <string>

namespace rsp::encoding::protobuf {

namespace {

// ---------------------------------------------------------------------------
// Hasher
// ---------------------------------------------------------------------------

// Wraps SHA256_CTX and provides typed feed helpers.  Each field's canonical
// bytes are pushed through feed() in field-number order.
class MessageHasher {
    SHA256_CTX ctx_;

public:
    MessageHasher() { SHA256_Init(&ctx_); }

    void feed(const void* data, size_t len) {
        SHA256_Update(&ctx_, data, len);
    }

    void feedUint8(uint8_t v) {
        feed(&v, 1);
    }

    // Feed a 32-bit unsigned integer big-endian.
    void feedUint32(uint32_t v) {
        const uint8_t buf[4] = {
            static_cast<uint8_t>((v >> 24) & 0xFFU),
            static_cast<uint8_t>((v >> 16) & 0xFFU),
            static_cast<uint8_t>((v >>  8) & 0xFFU),
            static_cast<uint8_t>( v        & 0xFFU),
        };
        feed(buf, 4);
    }

    void feedInt32(int32_t v) { feedUint32(static_cast<uint32_t>(v)); }

    // Feed a 64-bit unsigned integer big-endian.
    void feedUint64(uint64_t v) {
        const uint8_t buf[8] = {
            static_cast<uint8_t>((v >> 56) & 0xFFU),
            static_cast<uint8_t>((v >> 48) & 0xFFU),
            static_cast<uint8_t>((v >> 40) & 0xFFU),
            static_cast<uint8_t>((v >> 32) & 0xFFU),
            static_cast<uint8_t>((v >> 24) & 0xFFU),
            static_cast<uint8_t>((v >> 16) & 0xFFU),
            static_cast<uint8_t>((v >>  8) & 0xFFU),
            static_cast<uint8_t>( v        & 0xFFU),
        };
        feed(buf, 8);
    }

    void feedBool(bool v) { feedUint8(v ? 1 : 0); }

    // Feed a length-prefixed byte string (bytes or string proto type).
    void feedBytes(const std::string& s) {
        feedUint32(static_cast<uint32_t>(s.size()));
        feed(s.data(), s.size());
    }

    // Feed the proto field number as a uint32 tag before each field value.
    void tag(uint32_t field_number) { feedUint32(field_number); }

    MessageHash finalize() {
        MessageHash result{};
        SHA256_Final(result.data(), &ctx_);
        return result;
    }
};

// ---------------------------------------------------------------------------
// Leaf / wrapper message types
// ---------------------------------------------------------------------------

static void hashNodeId(MessageHasher& h, const rsp::proto::NodeId& id) {
    h.tag(1); h.feedBytes(id.value());
}

static void hashSocketId(MessageHasher& h, const rsp::proto::SocketID& id) {
    h.tag(1); h.feedBytes(id.value());
}

static void hashUuid(MessageHasher& h, const rsp::proto::Uuid& uuid) {
    h.tag(1); h.feedBytes(uuid.value());
}

static void hashDateTime(MessageHasher& h, const rsp::proto::DateTime& dt) {
    h.tag(1); h.feedUint64(dt.milliseconds_since_epoch());
}

static void hashPublicKey(MessageHasher& h, const rsp::proto::PublicKey& pk) {
    h.tag(1); h.feedUint32(static_cast<uint32_t>(pk.algorithm()));
    h.tag(2); h.feedBytes(pk.public_key());
}

// Endorsements appear as content inside some submessages; their own
// signature field (99) is treated as data and included.
static void hashEndorsement(MessageHasher& h, const rsp::proto::Endorsement& e) {
    if (e.has_subject())            { h.tag(1);  hashNodeId(h, e.subject()); }
    if (e.has_endorsement_service()){ h.tag(2);  hashNodeId(h, e.endorsement_service()); }
    if (e.has_endorsement_type())   { h.tag(3);  hashUuid(h, e.endorsement_type()); }
    h.tag(4); h.feedBytes(e.endorsement_value());
    if (e.has_valid_until())        { h.tag(5);  hashDateTime(h, e.valid_until()); }
    h.tag(99); h.feedBytes(e.signature());
}

// ---------------------------------------------------------------------------
// Challenge / identity
// ---------------------------------------------------------------------------

static void hashChallengeRequest(MessageHasher& h,
                                 const rsp::proto::ChallengeRequest& m) {
    if (m.has_nonce()) { h.tag(1); hashUuid(h, m.nonce()); }
}

static void hashIdentity(MessageHasher& h, const rsp::proto::Identity& m) {
    if (m.has_nonce())      { h.tag(1); hashUuid(h, m.nonce()); }
    if (m.has_public_key()) { h.tag(2); hashPublicKey(h, m.public_key()); }
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

static void hashRouteEntry(MessageHasher& h, const rsp::proto::RouteEntry& e) {
    if (e.has_node_id())   { h.tag(1); hashNodeId(h, e.node_id()); }
    h.tag(2); h.feedUint32(e.hops_away());
    if (e.has_last_seen()) { h.tag(3); hashDateTime(h, e.last_seen()); }
}

static void hashRouteUpdate(MessageHasher& h, const rsp::proto::RouteUpdate& m) {
    // Note: proto field number for entries is 2, not 1.
    h.tag(2); h.feedUint32(static_cast<uint32_t>(m.entries_size()));
    for (int i = 0; i < m.entries_size(); ++i) {
        hashRouteEntry(h, m.entries(i));
    }
}

// ---------------------------------------------------------------------------
// Error / ping
// ---------------------------------------------------------------------------

static void hashError(MessageHasher& h, const rsp::proto::Error& m) {
    h.tag(1); h.feedUint32(static_cast<uint32_t>(m.error_code()));
    h.tag(2); h.feedBytes(m.message());
}

static void hashPingRequest(MessageHasher& h, const rsp::proto::PingRequest& m) {
    if (m.has_nonce())     { h.tag(1); hashUuid(h, m.nonce()); }
    h.tag(2); h.feedUint32(m.sequence());
    if (m.has_time_sent()) { h.tag(3); hashDateTime(h, m.time_sent()); }
}

static void hashPingReply(MessageHasher& h, const rsp::proto::PingReply& m) {
    if (m.has_nonce())        { h.tag(1); hashUuid(h, m.nonce()); }
    h.tag(2); h.feedUint32(m.sequence());
    if (m.has_time_sent())    { h.tag(3); hashDateTime(h, m.time_sent()); }
    if (m.has_time_replied()) { h.tag(4); hashDateTime(h, m.time_replied()); }
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

static void hashAddress(MessageHasher& h, const rsp::proto::Address& a) {
    h.tag(1); h.feedUint32(a.ipv4());
    h.tag(2); h.feedBytes(a.ipv6());
}

static void hashPortRange(MessageHasher& h, const rsp::proto::PortRange& p) {
    h.tag(1); h.feedUint32(p.start_port());
    h.tag(2); h.feedUint32(p.end_port());
}

static void hashResourceTCPConnect(MessageHasher& h,
                                   const rsp::proto::ResourceTCPConnect& m) {
    if (m.has_node_id()) { h.tag(1); hashNodeId(h, m.node_id()); }
    h.tag(2); h.feedUint32(static_cast<uint32_t>(m.source_addresses_size()));
    for (int i = 0; i < m.source_addresses_size(); ++i) {
        hashAddress(h, m.source_addresses(i));
    }
}

static void hashResourceTCPListen(MessageHasher& h,
                                  const rsp::proto::ResourceTCPListen& m) {
    if (m.has_node_id()) { h.tag(1); hashNodeId(h, m.node_id()); }
    h.tag(2); h.feedUint32(static_cast<uint32_t>(m.listen_address_size()));
    for (int i = 0; i < m.listen_address_size(); ++i) {
        hashAddress(h, m.listen_address(i));
    }
    if (m.has_allowed_range()) { h.tag(3); hashPortRange(h, m.allowed_range()); }
}

static void hashResourceRecord(MessageHasher& h,
                               const rsp::proto::ResourceRecord& r) {
    switch (r.resource_type_case()) {
    case rsp::proto::ResourceRecord::kTcpConnect:
        h.tag(1); hashResourceTCPConnect(h, r.tcp_connect());
        break;
    case rsp::proto::ResourceRecord::kTcpListen:
        h.tag(2); hashResourceTCPListen(h, r.tcp_listen());
        break;
    case rsp::proto::ResourceRecord::RESOURCE_TYPE_NOT_SET:
        break;
    }
}

static void hashResourceAdvertisement(MessageHasher& h,
                                      const rsp::proto::ResourceAdvertisement& m) {
    h.tag(1); h.feedUint32(static_cast<uint32_t>(m.records_size()));
    for (int i = 0; i < m.records_size(); ++i) {
        hashResourceRecord(h, m.records(i));
    }
}

static void hashResourceQuery(MessageHasher& h,
                              const rsp::proto::ResourceQuery& m) {
    h.tag(1); h.feedBytes(m.query());
    h.tag(2); h.feedUint32(m.max_records());
}

// ---------------------------------------------------------------------------
// Socket operations
// ---------------------------------------------------------------------------

static void hashSocketReply(MessageHasher& h, const rsp::proto::SocketReply& m) {
    if (m.has_socket_id())                 { h.tag(1); hashSocketId(h, m.socket_id()); }
    h.tag(2); h.feedUint32(static_cast<uint32_t>(m.error()));
    if (m.has_message())                   { h.tag(3); h.feedBytes(m.message()); }
    if (m.has_new_socket_remote_address()) { h.tag(4); h.feedBytes(m.new_socket_remote_address()); }
    if (m.has_new_socket_id())             { h.tag(5); hashSocketId(h, m.new_socket_id()); }
    if (m.has_socket_error_code())         { h.tag(6); h.feedInt32(m.socket_error_code()); }
    if (m.has_data())                      { h.tag(7); h.feedBytes(m.data()); }
}

static void hashConnectTCPRequest(MessageHasher& h,
                                  const rsp::proto::ConnectTCPRequest& m) {
    h.tag(1); h.feedBytes(m.host_port());
    if (m.has_socket_number()) { h.tag(2); hashSocketId(h, m.socket_number()); }
    if (m.has_reuse_addr())    { h.tag(3); h.feedBool(m.reuse_addr()); }
    if (m.has_source_port())   { h.tag(4); h.feedUint32(m.source_port()); }
    if (m.has_timeout_ms())    { h.tag(5); h.feedUint32(m.timeout_ms()); }
    if (m.has_retries())       { h.tag(6); h.feedUint32(m.retries()); }
    if (m.has_retry_ms())      { h.tag(7); h.feedUint32(m.retry_ms()); }
    if (m.has_async_data())    { h.tag(8); h.feedBool(m.async_data()); }
    if (m.has_use_socket())    { h.tag(9); h.feedBool(m.use_socket()); }
    if (m.has_share_socket())  { h.tag(10); h.feedBool(m.share_socket()); }
}

static void hashListenTCPRequest(MessageHasher& h,
                                 const rsp::proto::ListenTCPRequest& m) {
    h.tag(1); h.feedBytes(m.host_port());
    if (m.has_socket_number())          { h.tag(2); hashSocketId(h, m.socket_number()); }
    if (m.has_reuse_addr())             { h.tag(3); h.feedBool(m.reuse_addr()); }
    if (m.has_timeout_ms())             { h.tag(4); h.feedUint32(m.timeout_ms()); }
    if (m.has_async_accept())           { h.tag(5); h.feedBool(m.async_accept()); }
    if (m.has_share_listening_socket()) { h.tag(6); h.feedBool(m.share_listening_socket()); }
    if (m.has_share_child_sockets())    { h.tag(7); h.feedBool(m.share_child_sockets()); }
    if (m.has_children_use_socket())    { h.tag(8); h.feedBool(m.children_use_socket()); }
    if (m.has_children_async_data())    { h.tag(9); h.feedBool(m.children_async_data()); }
}

static void hashAcceptTCP(MessageHasher& h, const rsp::proto::AcceptTCP& m) {
    if (m.has_listen_socket_number()) { h.tag(1); hashSocketId(h, m.listen_socket_number()); }
    if (m.has_new_socket_number())    { h.tag(2); hashSocketId(h, m.new_socket_number()); }
    if (m.has_timeout_ms())           { h.tag(3); h.feedUint32(m.timeout_ms()); }
    if (m.has_share_child_socket())   { h.tag(4); h.feedBool(m.share_child_socket()); }
    if (m.has_child_use_socket())     { h.tag(5); h.feedBool(m.child_use_socket()); }
    if (m.has_child_async_data())     { h.tag(6); h.feedBool(m.child_async_data()); }
}

static void hashSocketSend(MessageHasher& h, const rsp::proto::SocketSend& m) {
    // Hash by field number order (1, 2, 3) regardless of proto source order.
    if (m.has_socket_number()) { h.tag(1); hashSocketId(h, m.socket_number()); }
    h.tag(2); h.feedBytes(m.data());
    h.tag(3); h.feedUint64(m.index());
}

static void hashSocketRecv(MessageHasher& h, const rsp::proto::SocketRecv& m) {
    // Hash by field number order (1, 2, 3, 4).
    if (m.has_socket_number()) { h.tag(1); hashSocketId(h, m.socket_number()); }
    if (m.has_max_bytes())     { h.tag(2); h.feedUint32(m.max_bytes()); }
    h.tag(3); h.feedUint64(m.index());
    if (m.has_wait_ms())       { h.tag(4); h.feedUint32(m.wait_ms()); }
}

static void hashSocketClose(MessageHasher& h, const rsp::proto::SocketClose& m) {
    if (m.has_socket_number()) { h.tag(1); hashSocketId(h, m.socket_number()); }
}

// ---------------------------------------------------------------------------
// Endorsement protocol messages
// ---------------------------------------------------------------------------

static void hashBeginEndorsementRequest(MessageHasher& h,
                                        const rsp::proto::BeginEndorsementRequest& m) {
    if (m.has_requested_values()) { h.tag(1); hashEndorsement(h, m.requested_values()); }
    if (m.has_auth_data())        { h.tag(2); h.feedBytes(m.auth_data()); }
}

static void hashEndorsementChallenge(MessageHasher& h,
                                     const rsp::proto::EndorsementChallenge& m) {
    if (m.has_stage())     { h.tag(1); h.feedUint32(m.stage()); }
    if (m.has_challenge()) { h.tag(2); h.feedBytes(m.challenge()); }
}

static void hashEndorsementChallengeReply(MessageHasher& h,
                                          const rsp::proto::EndorsementChallengeReply& m) {
    if (m.has_stage())           { h.tag(1); h.feedUint32(m.stage()); }
    if (m.has_challenge_reply()) { h.tag(2); h.feedBytes(m.challenge_reply()); }
}

static void hashEndorsementDone(MessageHasher& h,
                                const rsp::proto::EndorsementDone& m) {
    h.tag(1); h.feedUint32(static_cast<uint32_t>(m.status()));
    if (m.has_new_endorsement()) { h.tag(2); hashEndorsement(h, m.new_endorsement()); }
}

// ---------------------------------------------------------------------------
// Top-level RSPMessage
// ---------------------------------------------------------------------------

static void hashRSPMessage(MessageHasher& h, const rsp::proto::RSPMessage& msg) {
    // field 1: optional NodeId destination
    if (msg.has_destination()) { h.tag(1); hashNodeId(h, msg.destination()); }

    // field 2: NodeId source
    if (msg.has_source()) { h.tag(2); hashNodeId(h, msg.source()); }

    // fields 3-21: oneof submessage — feed the active field's tag as discriminator
    switch (msg.submessage_case()) {
    case rsp::proto::RSPMessage::kChallengeRequest:
        h.tag(3);  hashChallengeRequest(h, msg.challenge_request()); break;
    case rsp::proto::RSPMessage::kIdentity:
        h.tag(4);  hashIdentity(h, msg.identity()); break;
    case rsp::proto::RSPMessage::kRoute:
        h.tag(5);  hashRouteUpdate(h, msg.route()); break;
    case rsp::proto::RSPMessage::kError:
        h.tag(6);  hashError(h, msg.error()); break;
    case rsp::proto::RSPMessage::kPingRequest:
        h.tag(7);  hashPingRequest(h, msg.ping_request()); break;
    case rsp::proto::RSPMessage::kPingReply:
        h.tag(8);  hashPingReply(h, msg.ping_reply()); break;
    case rsp::proto::RSPMessage::kConnectTcpRequest:
        h.tag(9);  hashConnectTCPRequest(h, msg.connect_tcp_request()); break;
    case rsp::proto::RSPMessage::kSocketReply:
        h.tag(10); hashSocketReply(h, msg.socket_reply()); break;
    case rsp::proto::RSPMessage::kSocketSend:
        h.tag(11); hashSocketSend(h, msg.socket_send()); break;
    case rsp::proto::RSPMessage::kSocketRecv:
        h.tag(12); hashSocketRecv(h, msg.socket_recv()); break;
    case rsp::proto::RSPMessage::kSocketClose:
        h.tag(13); hashSocketClose(h, msg.socket_close()); break;
    case rsp::proto::RSPMessage::kListenTcpRequest:
        h.tag(14); hashListenTCPRequest(h, msg.listen_tcp_request()); break;
    case rsp::proto::RSPMessage::kAcceptTcp:
        h.tag(15); hashAcceptTCP(h, msg.accept_tcp()); break;
    case rsp::proto::RSPMessage::kResourceAdvertisement:
        h.tag(16); hashResourceAdvertisement(h, msg.resource_advertisement()); break;
    case rsp::proto::RSPMessage::kResourceQuery:
        h.tag(17); hashResourceQuery(h, msg.resource_query()); break;
    case rsp::proto::RSPMessage::kBeginEndorsementRequest:
        h.tag(18); hashBeginEndorsementRequest(h, msg.begin_endorsement_request()); break;
    case rsp::proto::RSPMessage::kEndorsementChallenge:
        h.tag(19); hashEndorsementChallenge(h, msg.endorsement_challenge()); break;
    case rsp::proto::RSPMessage::kEndorsementChallengeReply:
        h.tag(20); hashEndorsementChallengeReply(h, msg.endorsement_challenge_reply()); break;
    case rsp::proto::RSPMessage::kEndorsementDone:
        h.tag(21); hashEndorsementDone(h, msg.endorsement_done()); break;
    case rsp::proto::RSPMessage::SUBMESSAGE_NOT_SET:
        break;
    }

    // field 99: SignatureBlock signature — EXCLUDED (this is the data being signed)

    // field 100: repeated Endorsement endorsements
    h.tag(100); h.feedUint32(static_cast<uint32_t>(msg.endorsements_size()));
    for (int i = 0; i < msg.endorsements_size(); ++i) {
        hashEndorsement(h, msg.endorsements(i));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MessageHash computeMessageHash(const rsp::proto::RSPMessage& message) {
    MessageHasher h;
    hashRSPMessage(h, message);
    return h.finalize();
}

}  // namespace rsp::encoding::protobuf

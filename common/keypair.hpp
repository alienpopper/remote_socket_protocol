#pragma once

#include "common/base_types.hpp"
#include "messages.pb.h"

#include <memory>
#include <string>

typedef struct evp_pkey_st EVP_PKEY;

namespace rsp {

class KeyPair {
public:
    KeyPair();
    explicit KeyPair(EVP_PKEY* key);

    KeyPair(KeyPair&& other) noexcept;
    KeyPair& operator=(KeyPair&& other) noexcept;

    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;

    ~KeyPair();

    static KeyPair generateP256();
    static KeyPair fromPublicKey(const rsp::proto::PublicKey& publicKey);
    static KeyPair readFromDisk(const std::string& privateKeyPath, const std::string& publicKeyPath);
    // Loads the private key from privateKeyPath if it exists; otherwise generates
    // a new P-256 key pair, saves the private key to that path, and returns it.
    static KeyPair loadOrGenerate(const std::string& privateKeyPath);
    static NodeID nodeIDFromPublicKeyFile(const std::string& publicKeyPath);
    KeyPair duplicate() const;

    NodeID nodeID() const;
    rsp::proto::PublicKey publicKey() const;
    Buffer sign(const Buffer& message) const;
    rsp::proto::SignatureBlock signBlock(const Buffer& message) const;
    bool verify(const Buffer& message, const Buffer& signature) const;
    bool verifyBlock(const Buffer& message, const rsp::proto::SignatureBlock& signatureBlock) const;
    void writeToDisk(const std::string& privateKeyPath, const std::string& publicKeyPath) const;

    bool isValid() const;
    bool hasPrivateKey() const;
    EVP_PKEY* get() const;

private:
    struct KeyDeleter {
        void operator()(EVP_PKEY* key) const;
    };

    static void ensureParentDirectory(const std::string& path);
    static std::unique_ptr<EVP_PKEY, KeyDeleter> duplicateKey(EVP_PKEY* key);
    static rsp::proto::SIGNATURE_ALGORITHMS algorithmForKey(EVP_PKEY* key);
    static void verifyP256(EVP_PKEY* key);
    static void verifyMatchingPair(EVP_PKEY* privateKey, EVP_PKEY* publicKey);

    std::unique_ptr<EVP_PKEY, KeyDeleter> key_;
};

}  // namespace rsp
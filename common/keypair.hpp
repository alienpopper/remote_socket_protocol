#pragma once

#include "common/base_types.hpp"

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
    static KeyPair readFromDisk(const std::string& privateKeyPath, const std::string& publicKeyPath);

    NodeID nodeID() const;
    Buffer sign(const Buffer& message) const;
    bool verify(const Buffer& message, const Buffer& signature) const;
    void writeToDisk(const std::string& privateKeyPath, const std::string& publicKeyPath) const;

    bool isValid() const;
    EVP_PKEY* get() const;

private:
    struct KeyDeleter {
        void operator()(EVP_PKEY* key) const;
    };

    static void ensureParentDirectory(const std::string& path);
    static std::unique_ptr<EVP_PKEY, KeyDeleter> duplicateKey(EVP_PKEY* key);
    static void verifyP256(EVP_PKEY* key);
    static void verifyMatchingPair(EVP_PKEY* privateKey, EVP_PKEY* publicKey);

    std::unique_ptr<EVP_PKEY, KeyDeleter> key_;
};

}  // namespace rsp
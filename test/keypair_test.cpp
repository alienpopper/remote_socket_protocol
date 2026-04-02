#include "common/keypair.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <openssl/bio.h>
#include <openssl/pem.h>

namespace {

std::string serializePublicKey(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        throw std::runtime_error("failed to allocate BIO");
    }

    if (PEM_write_bio_PUBKEY(bio, key) != 1) {
        BIO_free(bio);
        throw std::runtime_error("failed to serialize public key");
    }

    BUF_MEM* buffer = nullptr;
    BIO_get_mem_ptr(bio, &buffer);
    if (buffer == nullptr) {
        BIO_free(bio);
        throw std::runtime_error("failed to read serialized public key");
    }

    const std::string serialized(buffer->data, buffer->length);
    BIO_free(bio);
    return serialized;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        rsp::KeyPair emptyKeyPair;
        require(!emptyKeyPair.isValid(), "default keypair should be invalid");

        rsp::KeyPair generatedKeyPair = rsp::KeyPair::generateP256();
        require(generatedKeyPair.isValid(), "generated keypair should be valid");
        require(generatedKeyPair.get() != nullptr, "generated keypair should expose an EVP_PKEY");

        const rsp::NodeID generatedNodeId = generatedKeyPair.nodeID();
        require(generatedNodeId != rsp::NodeID(), "generated NodeID should not be zero");

        const std::filesystem::path testDir = std::filesystem::path("build") / "test" / "keypair";
        const std::filesystem::path privateKeyPath = testDir / "private.pem";
        const std::filesystem::path publicKeyPath = testDir / "public.pem";

        generatedKeyPair.writeToDisk(privateKeyPath.string(), publicKeyPath.string());
        require(std::filesystem::exists(privateKeyPath), "private key file should exist after write");
        require(std::filesystem::exists(publicKeyPath), "public key file should exist after write");

        rsp::KeyPair loadedKeyPair = rsp::KeyPair::readFromDisk(privateKeyPath.string(), publicKeyPath.string());
        require(loadedKeyPair.isValid(), "loaded keypair should be valid");
        require(loadedKeyPair.nodeID() == generatedNodeId,
            "loaded keypair should hash to the same NodeID as the generated keypair");

        require(serializePublicKey(generatedKeyPair.get()) == serializePublicKey(loadedKeyPair.get()),
                "loaded public key should match generated public key");

        std::filesystem::remove(privateKeyPath);
        std::filesystem::remove(publicKeyPath);
        std::filesystem::remove_all(testDir.parent_path());

        std::cout << "keypair_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "keypair_test failed: " << exception.what() << '\n';
        return 1;
    }
}
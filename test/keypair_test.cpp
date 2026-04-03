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

        const uint8_t messageBytes[] = {0x01, 0x02, 0x03, 0x04};
        rsp::Buffer message(messageBytes, 4);
        rsp::Buffer signature = generatedKeyPair.sign(message);
        require(!signature.empty(), "generated signature should not be empty");
        require(generatedKeyPair.verify(message, signature), "generated signature should verify");

        const rsp::NodeID generatedNodeId = generatedKeyPair.nodeID();
        require(generatedNodeId != rsp::NodeID(), "generated NodeID should not be zero");

        const rsp::proto::PublicKey exportedPublicKey = generatedKeyPair.publicKey();
        require(exportedPublicKey.algorithm() == rsp::proto::P256, "exported public key should report P256");
        require(!exportedPublicKey.public_key().empty(), "exported public key bytes should not be empty");

        rsp::KeyPair publicOnlyKeyPair = rsp::KeyPair::fromPublicKey(exportedPublicKey);
        require(publicOnlyKeyPair.isValid(), "public-only keypair should be valid");
        require(!publicOnlyKeyPair.hasPrivateKey(), "public-only keypair should not expose a private component");
        require(publicOnlyKeyPair.nodeID() == generatedNodeId,
            "public-only keypair should hash to the same NodeID as the original keypair");
        require(publicOnlyKeyPair.verify(message, signature),
            "public-only keypair should verify signatures from the original keypair");

        bool publicOnlySignThrown = false;
        try {
            static_cast<void>(publicOnlyKeyPair.sign(message));
        } catch (const std::runtime_error&) {
            publicOnlySignThrown = true;
        }
        require(publicOnlySignThrown, "public-only keypair should reject signing");

        const rsp::proto::SignatureBlock signatureBlock = generatedKeyPair.signBlock(message);
        require(signatureBlock.algorithm() == rsp::proto::P256, "signature block should record the P256 algorithm");
        require(signatureBlock.signer().value().size() == 16, "signature block signer should contain a NodeId");
        require(!signatureBlock.signature().empty(), "signature block should contain signature bytes");
        require(generatedKeyPair.verifyBlock(message, signatureBlock),
            "generated keypair should verify its own signature block");
        require(publicOnlyKeyPair.verifyBlock(message, signatureBlock),
            "public-only keypair should verify signature blocks from the original keypair");
        require(publicOnlyKeyPair.verify(message,
                         rsp::Buffer(reinterpret_cast<const uint8_t*>(signatureBlock.signature().data()),
                                 static_cast<uint32_t>(signatureBlock.signature().size()))),
            "signature block bytes should verify with the public-only keypair");

        rsp::proto::SignatureBlock wrongSignerBlock = signatureBlock;
        wrongSignerBlock.mutable_signer()->set_value(std::string(16, '\0'));
        require(!generatedKeyPair.verifyBlock(message, wrongSignerBlock),
            "signature block with the wrong signer should fail verification");

        rsp::proto::SignatureBlock wrongAlgorithmBlock = signatureBlock;
        wrongAlgorithmBlock.set_algorithm(rsp::proto::RSA2048);
        require(!generatedKeyPair.verifyBlock(message, wrongAlgorithmBlock),
            "signature block with the wrong algorithm should fail verification");

        message.data()[0] ^= 0xFF;
        require(!generatedKeyPair.verify(message, signature), "tampered message should fail verification");
        require(!generatedKeyPair.verifyBlock(message, signatureBlock),
            "tampered message should fail signature block verification");
        message.data()[0] ^= 0xFF;

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
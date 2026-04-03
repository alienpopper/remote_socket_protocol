#include "common/keypair.hpp"

#include "os/os_fileops.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <openssl/bio.h>
#include <openssl/buf.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

namespace rsp {

namespace {

std::runtime_error makeError(const std::string& message) {
    return std::runtime_error(message);
}

EVP_PKEY* readPrivateKeyFile(const std::string& path) {
    FILE* file = rsp::os::openFile(path, "rb");
    if (file == nullptr) {
        throw makeError("failed to open private key file: " + path);
    }

    EVP_PKEY* key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
    std::fclose(file);

    if (key == nullptr) {
        throw makeError("failed to parse private key file: " + path);
    }

    return key;
}

EVP_PKEY* readPublicKeyFile(const std::string& path) {
    FILE* file = rsp::os::openFile(path, "rb");
    if (file == nullptr) {
        throw makeError("failed to open public key file: " + path);
    }

    EVP_PKEY* key = PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
    std::fclose(file);

    if (key == nullptr) {
        throw makeError("failed to parse public key file: " + path);
    }

    return key;
}

EVP_PKEY* readPublicKeyBuffer(const std::string& publicKeyBytes) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(publicKeyBytes.data(), static_cast<int>(publicKeyBytes.size())),
                                                  &BIO_free);
    if (!bio) {
        throw makeError("failed to allocate BIO for public key");
    }

    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    if (key == nullptr) {
        throw makeError("failed to parse public key bytes");
    }

    return key;
}

std::string writePublicKeyBytes(EVP_PKEY* key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!bio) {
        throw makeError("failed to allocate BIO for public key serialization");
    }

    if (PEM_write_bio_PUBKEY(bio.get(), key) != 1) {
        throw makeError("failed to serialize public key");
    }

    BUF_MEM* buffer = nullptr;
    BIO_get_mem_ptr(bio.get(), &buffer);
    if (buffer == nullptr) {
        throw makeError("failed to read serialized public key bytes");
    }

    return std::string(buffer->data, buffer->length);
}

std::string serializeNodeIdBytes(const NodeID& nodeId) {
    std::string bytes;
    bytes.reserve(16);

    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((nodeId.high() >> shift) & 0xFFULL));
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((nodeId.low() >> shift) & 0xFFULL));
    }

    return bytes;
}

NodeID deserializeNodeIdBytes(const std::string& value) {
    if (value.size() != 16) {
        throw makeError("signature block signer has an invalid NodeID length");
    }

    uint64_t high = 0;
    uint64_t low = 0;

    for (int index = 0; index < 8; ++index) {
        high = (high << 8) | static_cast<uint64_t>(static_cast<unsigned char>(value[static_cast<size_t>(index)]));
    }

    for (int index = 8; index < 16; ++index) {
        low = (low << 8) | static_cast<uint64_t>(static_cast<unsigned char>(value[static_cast<size_t>(index)]));
    }

    return NodeID(high, low);
}

void writePrivateKeyFile(const std::string& path, EVP_PKEY* key) {
    FILE* file = rsp::os::openFile(path, "wb");
    if (file == nullptr) {
        throw makeError("failed to open private key file for writing: " + path);
    }

    const int result = PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(file);

    if (result != 1) {
        throw makeError("failed to write private key file: " + path);
    }
}

void writePublicKeyFile(const std::string& path, EVP_PKEY* key) {
    FILE* file = rsp::os::openFile(path, "wb");
    if (file == nullptr) {
        throw makeError("failed to open public key file for writing: " + path);
    }

    const int result = PEM_write_PUBKEY(file, key);
    std::fclose(file);

    if (result != 1) {
        throw makeError("failed to write public key file: " + path);
    }
}

}  // namespace

KeyPair::KeyPair() = default;

KeyPair::KeyPair(EVP_PKEY* key) : key_(key) {
    verifyP256(key_.get());
}

KeyPair::KeyPair(KeyPair&& other) noexcept = default;

KeyPair& KeyPair::operator=(KeyPair&& other) noexcept = default;

KeyPair::~KeyPair() = default;

KeyPair KeyPair::generateP256() {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (context == nullptr) {
        throw makeError("failed to allocate EVP_PKEY_CTX");
    }

    EVP_PKEY* key = nullptr;
    const bool success = EVP_PKEY_keygen_init(context) == 1 &&
                         EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) == 1 &&
                         EVP_PKEY_keygen(context, &key) == 1;

    EVP_PKEY_CTX_free(context);

    if (!success || key == nullptr) {
        if (key != nullptr) {
            EVP_PKEY_free(key);
        }

        throw makeError("failed to generate P-256 keypair");
    }

    return KeyPair(key);
}

KeyPair KeyPair::fromPublicKey(const rsp::proto::PublicKey& publicKey) {
    if (publicKey.algorithm() != rsp::proto::P256) {
        throw makeError("unsupported public key algorithm");
    }

    if (publicKey.public_key().empty()) {
        throw makeError("public key bytes are empty");
    }

    std::unique_ptr<EVP_PKEY, KeyDeleter> key(readPublicKeyBuffer(publicKey.public_key()));
    verifyP256(key.get());
    return KeyPair(duplicateKey(key.get()).release());
}

KeyPair KeyPair::readFromDisk(const std::string& privateKeyPath, const std::string& publicKeyPath) {
    std::unique_ptr<EVP_PKEY, KeyDeleter> privateKey(readPrivateKeyFile(privateKeyPath));
    std::unique_ptr<EVP_PKEY, KeyDeleter> publicKey(readPublicKeyFile(publicKeyPath));

    verifyP256(privateKey.get());
    verifyP256(publicKey.get());
    verifyMatchingPair(privateKey.get(), publicKey.get());

    return KeyPair(duplicateKey(privateKey.get()).release());
}

KeyPair KeyPair::duplicate() const {
    if (!isValid()) {
        throw makeError("cannot duplicate an empty keypair");
    }

    return KeyPair(duplicateKey(key_.get()).release());
}

NodeID KeyPair::nodeID() const {
    if (!isValid()) {
        throw makeError("cannot derive a NodeID from an empty keypair");
    }

    unsigned char* derBytes = nullptr;
    const int derLength = i2d_PUBKEY(key_.get(), &derBytes);
    if (derLength <= 0 || derBytes == nullptr) {
        throw makeError("failed to serialize public key for NodeID hashing");
    }

    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    const unsigned char* derBegin = derBytes;
    const int hashResult = SHA256(derBegin, static_cast<size_t>(derLength), digest) == nullptr ? 0 : 1;
    OPENSSL_free(derBytes);

    if (hashResult != 1) {
        throw makeError("failed to hash public key into NodeID");
    }

    uint64_t high = 0;
    uint64_t low = 0;
    for (int index = 0; index < 8; ++index) {
        high = (high << 8) | static_cast<uint64_t>(digest[index]);
        low = (low << 8) | static_cast<uint64_t>(digest[index + 8]);
    }

    return NodeID(high, low);
}

rsp::proto::PublicKey KeyPair::publicKey() const {
    if (!isValid()) {
        throw makeError("cannot export a public key from an empty keypair");
    }

    rsp::proto::PublicKey publicKey;
    publicKey.set_algorithm(algorithmForKey(key_.get()));
    publicKey.set_public_key(writePublicKeyBytes(key_.get()));
    return publicKey;
}

Buffer KeyPair::sign(const Buffer& message) const {
    if (!isValid()) {
        throw makeError("cannot sign with an empty keypair");
    }

    if (!hasPrivateKey()) {
        throw makeError("cannot sign with a public-only keypair");
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context) {
        throw makeError("failed to allocate signing context");
    }

    if (EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key_.get()) != 1) {
        throw makeError("failed to initialize signature generation");
    }

    if (!message.empty() && EVP_DigestSignUpdate(context.get(), message.data(), message.size()) != 1) {
        throw makeError("failed to update signature generation");
    }

    size_t signatureLength = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &signatureLength) != 1) {
        throw makeError("failed to size signature buffer");
    }

    Buffer signature(static_cast<uint32_t>(signatureLength));
    if (EVP_DigestSignFinal(context.get(), signature.data(), &signatureLength) != 1) {
        throw makeError("failed to finalize signature generation");
    }

    signature.resize(static_cast<uint32_t>(signatureLength));
    return signature;
}

rsp::proto::SignatureBlock KeyPair::signBlock(const Buffer& message) const {
    const Buffer signature = sign(message);
    const NodeID signer = nodeID();

    rsp::proto::SignatureBlock signatureBlock;
    signatureBlock.mutable_signer()->set_value(serializeNodeIdBytes(signer));
    signatureBlock.set_algorithm(algorithmForKey(key_.get()));
    signatureBlock.set_signature(std::string(reinterpret_cast<const char*>(signature.data()), signature.size()));
    return signatureBlock;
}

bool KeyPair::verify(const Buffer& message, const Buffer& signature) const {
    if (!isValid()) {
        throw makeError("cannot verify with an empty keypair");
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context) {
        throw makeError("failed to allocate verification context");
    }

    if (EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key_.get()) != 1) {
        throw makeError("failed to initialize signature verification");
    }

    if (!message.empty() && EVP_DigestVerifyUpdate(context.get(), message.data(), message.size()) != 1) {
        throw makeError("failed to update signature verification");
    }

    const int verifyResult = EVP_DigestVerifyFinal(context.get(), signature.data(), signature.size());
    if (verifyResult == 1) {
        return true;
    }

    if (verifyResult == 0) {
        return false;
    }

    throw makeError("failed to finalize signature verification");
}

bool KeyPair::verifyBlock(const Buffer& message, const rsp::proto::SignatureBlock& signatureBlock) const {
    if (!isValid()) {
        throw makeError("cannot verify with an empty keypair");
    }

    if (signatureBlock.algorithm() != algorithmForKey(key_.get())) {
        return false;
    }

    if (deserializeNodeIdBytes(signatureBlock.signer().value()) != nodeID()) {
        return false;
    }

    const Buffer signature(reinterpret_cast<const uint8_t*>(signatureBlock.signature().data()),
                           static_cast<uint32_t>(signatureBlock.signature().size()));
    return verify(message, signature);
}

void KeyPair::writeToDisk(const std::string& privateKeyPath, const std::string& publicKeyPath) const {
    if (!isValid()) {
        throw makeError("cannot write an empty keypair");
    }

    if (!hasPrivateKey()) {
        throw makeError("cannot write a private key for a public-only keypair");
    }

    ensureParentDirectory(privateKeyPath);
    ensureParentDirectory(publicKeyPath);
    writePrivateKeyFile(privateKeyPath, key_.get());
    writePublicKeyFile(publicKeyPath, key_.get());
}

bool KeyPair::isValid() const {
    return key_ != nullptr;
}

bool KeyPair::hasPrivateKey() const {
    if (!isValid()) {
        return false;
    }

    EC_KEY* ecKey = EVP_PKEY_get1_EC_KEY(key_.get());
    if (ecKey == nullptr) {
        return false;
    }

    const BIGNUM* privateKey = EC_KEY_get0_private_key(ecKey);
    const bool hasPrivateComponent = privateKey != nullptr;
    EC_KEY_free(ecKey);
    return hasPrivateComponent;
}

EVP_PKEY* KeyPair::get() const {
    return key_.get();
}

void KeyPair::KeyDeleter::operator()(EVP_PKEY* key) const {
    if (key != nullptr) {
        EVP_PKEY_free(key);
    }
}

void KeyPair::ensureParentDirectory(const std::string& path) {
    const std::filesystem::path filePath(path);
    const std::filesystem::path parentPath = filePath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }
}

std::unique_ptr<EVP_PKEY, KeyPair::KeyDeleter> KeyPair::duplicateKey(EVP_PKEY* key) {
    if (key == nullptr) {
        throw makeError("cannot duplicate a null key");
    }

    if (EVP_PKEY_up_ref(key) != 1) {
        throw makeError("failed to duplicate key reference");
    }

    return std::unique_ptr<EVP_PKEY, KeyDeleter>(key);
}

rsp::proto::SIGNATURE_ALGORITHMS KeyPair::algorithmForKey(EVP_PKEY* key) {
    verifyP256(key);
    return rsp::proto::P256;
}

void KeyPair::verifyP256(EVP_PKEY* key) {
    if (key == nullptr) {
        throw makeError("key is null");
    }

    if (EVP_PKEY_id(key) != EVP_PKEY_EC) {
        throw makeError("key is not an EC key");
    }

    EC_KEY* ecKey = EVP_PKEY_get1_EC_KEY(key);
    if (ecKey == nullptr) {
        throw makeError("failed to access EC key");
    }

    const EC_GROUP* group = EC_KEY_get0_group(ecKey);
    const int curve = group == nullptr ? NID_undef : EC_GROUP_get_curve_name(group);
    EC_KEY_free(ecKey);

    if (curve != NID_X9_62_prime256v1) {
        throw makeError("key is not on the P-256 curve");
    }
}

void KeyPair::verifyMatchingPair(EVP_PKEY* privateKey, EVP_PKEY* publicKey) {
    std::unique_ptr<BIO, decltype(&BIO_free)> privateBio(BIO_new(BIO_s_mem()), &BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> publicBio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!privateBio || !publicBio) {
        throw makeError("failed to allocate BIO");
    }

    if (PEM_write_bio_PUBKEY(privateBio.get(), privateKey) != 1 ||
        PEM_write_bio_PUBKEY(publicBio.get(), publicKey) != 1) {
        throw makeError("failed to serialize public keys for comparison");
    }

    BUF_MEM* privateBuffer = nullptr;
    BUF_MEM* publicBuffer = nullptr;
    BIO_get_mem_ptr(privateBio.get(), &privateBuffer);
    BIO_get_mem_ptr(publicBio.get(), &publicBuffer);

    if (privateBuffer == nullptr || publicBuffer == nullptr) {
        throw makeError("failed to read serialized key data");
    }

    if (privateBuffer->length != publicBuffer->length ||
        CRYPTO_memcmp(privateBuffer->data, publicBuffer->data, privateBuffer->length) != 0) {
        throw makeError("public key does not match private key");
    }
}

}  // namespace rsp
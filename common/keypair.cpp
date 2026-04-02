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

KeyPair KeyPair::readFromDisk(const std::string& privateKeyPath, const std::string& publicKeyPath) {
    std::unique_ptr<EVP_PKEY, KeyDeleter> privateKey(readPrivateKeyFile(privateKeyPath));
    std::unique_ptr<EVP_PKEY, KeyDeleter> publicKey(readPublicKeyFile(publicKeyPath));

    verifyP256(privateKey.get());
    verifyP256(publicKey.get());
    verifyMatchingPair(privateKey.get(), publicKey.get());

    return KeyPair(duplicateKey(privateKey.get()).release());
}

void KeyPair::writeToDisk(const std::string& privateKeyPath, const std::string& publicKeyPath) const {
    if (!isValid()) {
        throw makeError("cannot write an empty keypair");
    }

    ensureParentDirectory(privateKeyPath);
    ensureParentDirectory(publicKeyPath);
    writePrivateKeyFile(privateKeyPath, key_.get());
    writePublicKeyFile(publicKeyPath, key_.get());
}

bool KeyPair::isValid() const {
    return key_ != nullptr;
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
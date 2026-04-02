#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rsp {

class Buffer {
public:
    Buffer();
    explicit Buffer(uint32_t size);
    Buffer(const uint8_t* data, uint32_t size);

    Buffer(const Buffer& other);
    Buffer& operator=(const Buffer& other);

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    uint8_t* data();
    const uint8_t* data() const;
    uint32_t size() const;
    bool empty() const;

    void resize(uint32_t size);

private:
    std::unique_ptr<uint8_t[]> bytes_;
    uint32_t size_;
};

class GUID {
public:
    GUID();
    GUID(uint64_t high, uint64_t low);
    GUID(const std::string& guidString);
    GUID(const char* guidString);

    std::string toString() const;
    operator std::string() const;

    uint64_t high() const;
    uint64_t low() const;

    bool operator<(const GUID& other) const;
    bool operator==(const GUID& other) const;
    bool operator!=(const GUID& other) const;

private:
    static GUID parse(const std::string& guidString);
    static uint64_t parseHex64(const std::string& text);
    static std::string normalize(const std::string& guidString);

    uint64_t high_;
    uint64_t low_;
};

class NodeID : public GUID {
public:
    NodeID();
    NodeID(uint64_t high, uint64_t low);
    NodeID(const GUID& guid);
    NodeID(const std::string& guidString);
    NodeID(const char* guidString);
};

}  // namespace rsp
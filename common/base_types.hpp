#pragma once

#include <cstdint>
#include <string>

namespace rsp {

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
#pragma once

#include "common/node.hpp"

namespace rsp::resource_manager {

class ResourceManager : public rsp::RSPNode {
public:
    ResourceManager();

    int run() const override;
};

}  // namespace rsp::resource_manager
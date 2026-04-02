#include "resource_manager/resource_manager.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    rsp::resource_manager::ResourceManager resourceManager;
    return resourceManager.run();
}
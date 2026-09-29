#include "keyforge/common/version.hpp"
#include "keyforge/server/config.hpp"
#include "keyforge/server/server.hpp"

#include <iostream>

int main(int argc, char** argv) {
    const auto parsed = keyforge::server::parse_config(argc, argv);
    if (!parsed.ok) {
        std::cerr << "keyforge-server: " << parsed.error << "\n\n"
                  << keyforge::server::usage();
        return 2;
    }
    if (parsed.show_help) {
        std::cout << keyforge::server::usage();
        return 0;
    }
    if (parsed.show_version) {
        std::cout << "KeyForge " << keyforge::version() << '\n';
        return 0;
    }
    keyforge::server::Server server{parsed.config};
    std::string error;
    if (!server.initialize(error)) {
        std::cerr << "keyforge-server: " << error << '\n';
        return 1;
    }
    return server.run();
}

#pragma once

#include "keyforge/server/config.hpp"

#include <memory>
#include <string>

namespace keyforge::server {

class Server {
public:
    explicit Server(Config config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    [[nodiscard]] bool initialize(std::string& error);
    [[nodiscard]] int run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace keyforge::server

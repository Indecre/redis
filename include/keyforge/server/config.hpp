#pragma once

#include "keyforge/protocol/resp.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace keyforge::server {

enum class AppendFsync { always, everysec, no };

struct Config {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{6380};
    std::filesystem::path data_dir{"./data"};
    bool append_only{true};
    AppendFsync append_fsync{AppendFsync::everysec};
    std::size_t max_clients{10000U};
    std::size_t max_client_output_bytes{64U * 1024U * 1024U};
    resp::Limits protocol_limits{};
};

struct ConfigResult {
    bool ok{false};
    bool show_help{false};
    bool show_version{false};
    Config config;
    std::string error;
};

[[nodiscard]] ConfigResult parse_config(int argc, char** argv);
[[nodiscard]] std::string usage();

}  // namespace keyforge::server


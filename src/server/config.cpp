#include "keyforge/server/config.hpp"

#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace keyforge::server {
namespace {

template <typename Integer>
[[nodiscard]] bool parse_unsigned(std::string_view text, Integer& output) {
    static_assert(std::is_unsigned_v<Integer>);
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return false;
    }
    Integer parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    output = parsed;
    return true;
}

}  // namespace

ConfigResult parse_config(int argc, char** argv) {
    ConfigResult result;
    result.ok = true;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--help") {
            result.show_help = true;
            continue;
        }
        if (option == "--version") {
            result.show_version = true;
            continue;
        }
        if (index + 1 >= argc) {
            result.ok = false;
            result.error = "missing value for " + std::string{option};
            return result;
        }
        const std::string_view value{argv[++index]};
        if (option == "--bind") {
            result.config.bind_address = value;
        } else if (option == "--port") {
            std::uint16_t port = 0;
            if (!parse_unsigned(value, port) || port == 0) {
                result.ok = false;
                result.error = "--port must be an integer from 1 to 65535";
                return result;
            }
            result.config.port = port;
        } else if (option == "--data-dir") {
            if (value.empty()) {
                result.ok = false;
                result.error = "--data-dir must not be empty";
                return result;
            }
            result.config.data_dir = value;
        } else if (option == "--appendonly") {
            if (value != "yes" && value != "no") {
                result.ok = false;
                result.error = "--appendonly must be yes or no";
                return result;
            }
            result.config.append_only = value == "yes";
        } else if (option == "--appendfsync") {
            if (value == "always") {
                result.config.append_fsync = AppendFsync::always;
            } else if (value == "everysec") {
                result.config.append_fsync = AppendFsync::everysec;
            } else if (value == "no") {
                result.config.append_fsync = AppendFsync::no;
            } else {
                result.ok = false;
                result.error = "--appendfsync must be always, everysec, or no";
                return result;
            }
        } else if (option == "--max-clients") {
            if (!parse_unsigned(value, result.config.max_clients) ||
                result.config.max_clients == 0) {
                result.ok = false;
                result.error = "--max-clients must be a positive integer";
                return result;
            }
        } else if (option == "--max-bulk-bytes") {
            if (!parse_unsigned(value, result.config.protocol_limits.max_bulk_bytes) ||
                result.config.protocol_limits.max_bulk_bytes == 0) {
                result.ok = false;
                result.error = "--max-bulk-bytes must be a positive integer";
                return result;
            }
        } else if (option == "--max-client-buffer-bytes") {
            if (!parse_unsigned(value, result.config.protocol_limits.max_buffer_bytes) ||
                result.config.protocol_limits.max_buffer_bytes == 0) {
                result.ok = false;
                result.error = "--max-client-buffer-bytes must be a positive integer";
                return result;
            }
        } else if (option == "--max-client-output-bytes") {
            if (!parse_unsigned(value, result.config.max_client_output_bytes) ||
                result.config.max_client_output_bytes == 0) {
                result.ok = false;
                result.error = "--max-client-output-bytes must be a positive integer";
                return result;
            }
        } else {
            result.ok = false;
            result.error = "unknown option: " + std::string{option};
            return result;
        }
    }
    if (result.config.protocol_limits.max_bulk_bytes >
        result.config.protocol_limits.max_buffer_bytes) {
        result.ok = false;
        result.error = "--max-bulk-bytes cannot exceed --max-client-buffer-bytes";
    }
    return result;
}

std::string usage() {
    return
        "Usage: keyforge-server [options]\n"
        "  --bind ADDRESS                    bind address (default 127.0.0.1)\n"
        "  --port PORT                       TCP port (default 6380)\n"
        "  --data-dir PATH                   persistence directory (default ./data)\n"
        "  --appendonly yes|no               enable AOF (default yes)\n"
        "  --appendfsync always|everysec|no  durability policy (default everysec)\n"
        "  --max-clients COUNT               connection limit (default 10000)\n"
        "  --max-bulk-bytes BYTES            bulk-string limit (default 67108864)\n"
        "  --max-client-buffer-bytes BYTES   input limit (default 134217728)\n"
        "  --max-client-output-bytes BYTES   output limit (default 67108864)\n"
        "  --help                             show this help\n"
        "  --version                          show the version\n";
}

}  // namespace keyforge::server

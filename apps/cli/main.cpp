#include "keyforge/common/fd.hpp"
#include "keyforge/common/version.hpp"
#include "keyforge/protocol/resp.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    std::string port{"6380"};
    std::vector<std::string> command;
    bool help{false};
    bool version{false};
};

[[nodiscard]] std::string usage() {
    return "Usage: keyforge-cli [-h HOST] [-p PORT] [COMMAND [ARG ...]]\n"
           "With no command, reads whitespace-separated commands interactively.\n";
}

[[nodiscard]] bool valid_port(std::string_view text) {
    unsigned int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0U &&
           value <= 65535U;
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    int index = 1;
    for (; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            options.help = true;
            return options;
        }
        if (argument == "--version") {
            options.version = true;
            return options;
        }
        if (argument == "-h" || argument == "-p") {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + std::string{argument});
            }
            if (argument == "-h") {
                options.host = argv[++index];
            } else {
                options.port = argv[++index];
                if (!valid_port(options.port)) {
                    throw std::runtime_error("port must be an integer from 1 to 65535");
                }
            }
            continue;
        }
        break;
    }
    for (; index < argc; ++index) {
        options.command.emplace_back(argv[index]);
    }
    return options;
}

[[nodiscard]] keyforge::Fd connect_to(const Options& options) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(options.host.c_str(), options.port.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error("cannot resolve server: " + std::string{::gai_strerror(lookup)});
    }

    keyforge::Fd connected;
    for (const addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        keyforge::Fd candidate{
            ::socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol)};
        if (candidate && ::connect(candidate.get(), address->ai_addr, address->ai_addrlen) == 0) {
            connected = std::move(candidate);
            break;
        }
    }
    ::freeaddrinfo(addresses);
    if (!connected) {
        throw std::runtime_error("cannot connect to " + options.host + ':' + options.port + ": " +
                                 std::strerror(errno));
    }
    return connected;
}

void send_all(int socket, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("send failed: " + std::string{std::strerror(errno)});
    }
}

[[nodiscard]] keyforge::resp::Value receive_one(int socket, keyforge::resp::Parser& parser) {
    for (;;) {
        auto parsed = parser.next();
        if (parsed.status == keyforge::resp::ParseStatus::complete && parsed.value.has_value()) {
            return std::move(*parsed.value);
        }
        if (parsed.status == keyforge::resp::ParseStatus::invalid) {
            throw std::runtime_error("server sent invalid RESP: " + parsed.error);
        }
        char buffer[16384];
        const ssize_t count = ::recv(socket, buffer, sizeof(buffer), 0);
        if (count > 0) {
            if (!parser.feed(std::string_view{buffer, static_cast<std::size_t>(count)})) {
                throw std::runtime_error("server response exceeded the client limit");
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count == 0) {
            throw std::runtime_error("server closed the connection");
        }
        throw std::runtime_error("receive failed: " + std::string{std::strerror(errno)});
    }
}

void print_value(const keyforge::resp::Value& value, std::size_t depth = 0) {
    switch (value.type) {
        case keyforge::resp::Type::simple_string:
            std::cout << value.text << '\n';
            return;
        case keyforge::resp::Type::error:
            std::cout << "(error) " << value.text << '\n';
            return;
        case keyforge::resp::Type::integer:
            std::cout << "(integer) " << value.integer << '\n';
            return;
        case keyforge::resp::Type::bulk_string:
            std::cout.write(value.text.data(), static_cast<std::streamsize>(value.text.size()));
            std::cout << '\n';
            return;
        case keyforge::resp::Type::null_bulk_string:
        case keyforge::resp::Type::null_array:
            std::cout << "(nil)\n";
            return;
        case keyforge::resp::Type::array:
            for (std::size_t index = 0; index < value.elements.size(); ++index) {
                std::cout << std::string(depth * 2U, ' ') << index + 1U << ") ";
                print_value(value.elements[index], depth + 1U);
            }
            if (value.elements.empty()) {
                std::cout << "(empty array)\n";
            }
            return;
    }
}

[[nodiscard]] std::vector<std::string> split_interactive(std::string_view line) {
    std::istringstream stream{std::string{line}};
    std::vector<std::string> arguments;
    std::string argument;
    while (stream >> argument) {
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::cout << usage();
            return 0;
        }
        if (options.version) {
            std::cout << "KeyForge CLI " << keyforge::version() << '\n';
            return 0;
        }
        keyforge::Fd socket = connect_to(options);
        keyforge::resp::Parser parser;
        if (!options.command.empty()) {
            send_all(socket.get(), keyforge::resp::encode_command(options.command));
            print_value(receive_one(socket.get(), parser));
            return 0;
        }

        std::string line;
        while (std::cout << options.host << ':' << options.port << "> " &&
               std::getline(std::cin, line)) {
            auto command = split_interactive(line);
            if (command.empty()) {
                continue;
            }
            send_all(socket.get(), keyforge::resp::encode_command(command));
            const auto response = receive_one(socket.get(), parser);
            print_value(response);
            if (command.size() == 1U &&
                (command[0] == "QUIT" || command[0] == "quit")) {
                break;
            }
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "keyforge-cli: " << exception.what() << '\n';
        return 1;
    }
}

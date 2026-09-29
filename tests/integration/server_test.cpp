#include "keyforge/common/fd.hpp"
#include "keyforge/protocol/resp.hpp"

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint16_t reserve_local_port() {
    keyforge::Fd socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!socket) {
        throw std::runtime_error("socket failed: " + std::string{std::strerror(errno)});
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("bind failed");
    }
    socklen_t length = sizeof(address);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        throw std::runtime_error("getsockname failed");
    }
    return ntohs(address.sin_port);
}

class ServerProcess {
public:
    explicit ServerProcess(std::uint16_t port,
                           std::vector<std::string> extra_options = {"--appendonly", "no"})
        : port_(port) {
        const std::string port_text = std::to_string(port);
        process_ = ::fork();
        if (process_ < 0) {
            throw std::runtime_error("fork failed");
        }
        if (process_ == 0) {
            std::vector<std::string> arguments{KEYFORGE_SERVER_PATH, "--port", port_text};
            arguments.insert(arguments.end(), std::make_move_iterator(extra_options.begin()),
                             std::make_move_iterator(extra_options.end()));
            std::vector<char*> pointers;
            pointers.reserve(arguments.size() + 1U);
            for (auto& argument : arguments) {
                pointers.push_back(argument.data());
            }
            pointers.push_back(nullptr);
            ::execv(KEYFORGE_SERVER_PATH, pointers.data());
            _exit(127);
        }
    }

    ~ServerProcess() {
        if (process_ > 0) {
            (void)::kill(process_, SIGTERM);
            if (!wait_for_exit(std::chrono::seconds{1})) {
                (void)::kill(process_, SIGKILL);
                (void)::waitpid(process_, nullptr, 0);
            }
        }
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    [[nodiscard]] keyforge::Fd connect_client() const {
        const auto deadline = Clock::now() + std::chrono::seconds{2};
        while (Clock::now() < deadline) {
            keyforge::Fd socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
            if (!socket) {
                throw std::runtime_error("client socket failed");
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)) == 0) {
                return socket;
            }
            int status = 0;
            if (::waitpid(process_, &status, WNOHANG) == process_) {
                throw std::runtime_error("server exited before accepting connections");
            }
            (void)::poll(nullptr, 0, 10);
        }
        throw std::runtime_error("server startup timed out");
    }

    void terminate() { (void)::kill(process_, SIGTERM); }

    [[nodiscard]] bool wait_for_exit(std::chrono::milliseconds timeout) {
        if (process_ <= 0) {
            return true;
        }
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            int status = 0;
            const pid_t result = ::waitpid(process_, &status, WNOHANG);
            if (result == process_) {
                process_ = -1;
                exit_status_ = status;
                return true;
            }
            if (result < 0 && errno != EINTR) {
                return false;
            }
            (void)::poll(nullptr, 0, 10);
        }
        return false;
    }

    [[nodiscard]] int exit_status() const noexcept { return exit_status_; }

private:
    std::uint16_t port_;
    pid_t process_{-1};
    int exit_status_{-1};
};

class TempDirectory {
public:
    TempDirectory() {
        char pattern[] = "/tmp/keyforge-server-test-XXXXXX";
        const char* const created = ::mkdtemp(pattern);
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

class Client {
public:
    explicit Client(keyforge::Fd socket) : socket_(std::move(socket)) {}

    void send_bytes(std::string_view wire) {
        std::size_t offset = 0;
        while (offset < wire.size()) {
            const ssize_t count = ::send(socket_.get(), wire.data() + offset, wire.size() - offset,
                                         MSG_NOSIGNAL);
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

    [[nodiscard]] keyforge::resp::Value receive() {
        const auto deadline = Clock::now() + std::chrono::seconds{2};
        for (;;) {
            auto parsed = parser_.next();
            if (parsed.status == keyforge::resp::ParseStatus::complete && parsed.value.has_value()) {
                return std::move(*parsed.value);
            }
            if (parsed.status == keyforge::resp::ParseStatus::invalid) {
                throw std::runtime_error("invalid server response: " + parsed.error);
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                throw std::runtime_error("response timed out");
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            pollfd descriptor{socket_.get(), POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
            if (ready <= 0) {
                if (ready < 0 && errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("response poll timed out or failed");
            }
            char buffer[4096];
            const ssize_t count = ::recv(socket_.get(), buffer, sizeof(buffer), 0);
            if (count <= 0) {
                throw std::runtime_error("server closed before response completed");
            }
            if (!parser_.feed(std::string_view{buffer, static_cast<std::size_t>(count)})) {
                throw std::runtime_error("response exceeded parser limit");
            }
        }
    }

private:
    keyforge::Fd socket_;
    keyforge::resp::Parser parser_;
};

}  // namespace

TEST_CASE("actual server handles PING ECHO and QUIT") {
    ServerProcess server{reserve_local_port()};
    Client client{server.connect_client()};

    client.send_bytes(keyforge::resp::encode_command({"ping"}));
    CHECK(client.receive() == keyforge::resp::Value::simple("PONG"));

    client.send_bytes(keyforge::resp::encode_command({"ECHO", std::string{"a\0b", 3}}));
    CHECK(client.receive() == keyforge::resp::Value::bulk(std::string{"a\0b", 3}));

    client.send_bytes(keyforge::resp::encode_command({"QUIT"}));
    CHECK(client.receive() == keyforge::resp::Value::simple("OK"));
}

TEST_CASE("actual server handles byte fragments and pipelines") {
    ServerProcess server{reserve_local_port()};
    Client client{server.connect_client()};

    const std::string fragmented = keyforge::resp::encode_command({"ECHO", "fragmented"});
    for (const char byte : fragmented) {
        client.send_bytes(std::string_view{&byte, 1});
    }
    CHECK(client.receive() == keyforge::resp::Value::bulk("fragmented"));

    std::string pipeline;
    for (int index = 0; index < 100; ++index) {
        pipeline += keyforge::resp::encode_command({"PING", std::to_string(index)});
    }
    client.send_bytes(pipeline);
    for (int index = 0; index < 100; ++index) {
        CHECK(client.receive() == keyforge::resp::Value::bulk(std::to_string(index)));
    }
}

TEST_CASE("server exits cleanly on SIGTERM") {
    ServerProcess server{reserve_local_port()};
    Client client{server.connect_client()};
    client.send_bytes(keyforge::resp::encode_command({"PING"}));
    REQUIRE(client.receive() == keyforge::resp::Value::simple("PONG"));
    server.terminate();
    REQUIRE(server.wait_for_exit(std::chrono::seconds{2}));
    CHECK(WIFEXITED(server.exit_status()));
    CHECK(WEXITSTATUS(server.exit_status()) == 0);
}

TEST_CASE("server delivers pubsub messages between clients") {
    ServerProcess server{reserve_local_port()};
    Client subscriber{server.connect_client()};
    Client publisher{server.connect_client()};

    subscriber.send_bytes(keyforge::resp::encode_command({"SUBSCRIBE", "events"}));
    CHECK(subscriber.receive() ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("subscribe"),
                                        keyforge::resp::Value::bulk("events"),
                                        keyforge::resp::Value::number(1)}));

    publisher.send_bytes(keyforge::resp::encode_command({"PUBLISH", "events", "payload"}));
    CHECK(publisher.receive() == keyforge::resp::Value::number(1));
    CHECK(subscriber.receive() ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("message"),
                                        keyforge::resp::Value::bulk("events"),
                                        keyforge::resp::Value::bulk("payload")}));

    subscriber.send_bytes(keyforge::resp::encode_command({"GET", "blocked"}));
    const auto restricted = subscriber.receive();
    REQUIRE(restricted.type == keyforge::resp::Type::error);
    CHECK(restricted.text.starts_with("ERR only"));

    subscriber.send_bytes(keyforge::resp::encode_command({"UNSUBSCRIBE", "events"}));
    CHECK(subscriber.receive() ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("unsubscribe"),
                                        keyforge::resp::Value::bulk("events"),
                                        keyforge::resp::Value::number(0)}));
}

TEST_CASE("actual clients observe WATCH conflicts") {
    ServerProcess server{reserve_local_port()};
    Client first{server.connect_client()};
    Client second{server.connect_client()};

    first.send_bytes(keyforge::resp::encode_command({"WATCH", "shared"}));
    REQUIRE(first.receive() == keyforge::resp::Value::simple("OK"));
    first.send_bytes(keyforge::resp::encode_command({"MULTI"}));
    REQUIRE(first.receive() == keyforge::resp::Value::simple("OK"));
    first.send_bytes(keyforge::resp::encode_command({"SET", "result", "stale"}));
    REQUIRE(first.receive() == keyforge::resp::Value::simple("QUEUED"));

    second.send_bytes(keyforge::resp::encode_command({"SET", "shared", "changed"}));
    REQUIRE(second.receive() == keyforge::resp::Value::simple("OK"));
    first.send_bytes(keyforge::resp::encode_command({"EXEC"}));
    CHECK(first.receive() == keyforge::resp::Value::null_array());
}

TEST_CASE("actual server restart recovers values TTL and dependency graph") {
    TempDirectory directory;
    const std::vector<std::string> options{"--appendonly", "yes", "--appendfsync", "always",
                                           "--data-dir", directory.string()};
    const auto first_port = reserve_local_port();
    {
        ServerProcess server{first_port, options};
        Client client{server.connect_client()};
        for (const auto& command :
             std::vector<std::vector<std::string>>{{"SET", "parent", "source", "EX", "60"},
                                                   {"SET", "child", "derived"},
                                                   {"KFG.DEPEND", "child", "parent"}}) {
            client.send_bytes(keyforge::resp::encode_command(command));
            const auto reply = client.receive();
            REQUIRE(reply.type != keyforge::resp::Type::error);
        }
        server.terminate();
        REQUIRE(server.wait_for_exit(std::chrono::seconds{2}));
    }

    ServerProcess recovered{reserve_local_port(), options};
    Client client{recovered.connect_client()};
    client.send_bytes(keyforge::resp::encode_command({"GET", "parent"}));
    CHECK(client.receive() == keyforge::resp::Value::bulk("source"));
    client.send_bytes(keyforge::resp::encode_command({"PTTL", "parent"}));
    const auto ttl = client.receive();
    REQUIRE(ttl.type == keyforge::resp::Type::integer);
    CHECK(ttl.integer > 0);
    CHECK(ttl.integer <= 60'000);
    client.send_bytes(keyforge::resp::encode_command({"KFG.DEPS", "child"}));
    CHECK(client.receive() ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("parent")}));
}

TEST_CASE("server handles large replies and enforces protocol and output limits") {
    SECTION("large binary reply is drained through partial reads and writes") {
        ServerProcess server{reserve_local_port()};
        Client client{server.connect_client()};
        std::string payload(4U * 1024U * 1024U, 'x');
        payload[123] = '\0';
        client.send_bytes(keyforge::resp::encode_command({"ECHO", payload}));
        CHECK(client.receive() == keyforge::resp::Value::bulk(payload));
    }

    SECTION("bulk limit returns a protocol error") {
        ServerProcess server{
            reserve_local_port(),
            {"--appendonly", "no", "--max-bulk-bytes", "1024",
             "--max-client-buffer-bytes", "4096", "--max-client-output-bytes", "512"}};
        Client client{server.connect_client()};
        client.send_bytes(keyforge::resp::encode_command({"ECHO", std::string(1025, 'x')}));
        const auto reply = client.receive();
        REQUIRE(reply.type == keyforge::resp::Type::error);
        CHECK(reply.text.find("bulk string exceeds") != std::string::npos);
    }

    SECTION("output backpressure disconnects an oversized reply") {
        ServerProcess server{
            reserve_local_port(),
            {"--appendonly", "no", "--max-bulk-bytes", "1024",
             "--max-client-buffer-bytes", "4096", "--max-client-output-bytes", "512"}};
        Client client{server.connect_client()};
        client.send_bytes(keyforge::resp::encode_command({"ECHO", std::string(600, 'x')}));
        CHECK_THROWS(client.receive());
    }
}

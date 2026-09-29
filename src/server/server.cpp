#include "keyforge/server/server.hpp"

#include "keyforge/commands/engine.hpp"
#include "keyforge/common/fd.hpp"
#include "keyforge/persistence/aof.hpp"
#include "keyforge/protocol/resp.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace keyforge::server {
namespace {

constexpr std::uint64_t listener_token = 1U;
constexpr std::uint64_t signal_token = 2U;
constexpr int max_events = 128;

[[nodiscard]] std::string system_error(std::string_view operation) {
    return std::string{operation} + ": " + std::strerror(errno);
}

[[nodiscard]] bool make_nonblocking_and_cloexec(int descriptor, std::string& error) {
    const int status_flags = ::fcntl(descriptor, F_GETFL, 0);
    if (status_flags < 0 || ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        error = system_error("cannot make listener non-blocking");
        return false;
    }
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        error = system_error("cannot make listener close-on-exec");
        return false;
    }
    return true;
}

[[nodiscard]] bool command_arguments(const resp::Value& frame,
                                     std::vector<std::string>& arguments) {
    if (frame.type != resp::Type::array || frame.elements.empty()) {
        return false;
    }
    arguments.clear();
    arguments.reserve(frame.elements.size());
    for (const auto& element : frame.elements) {
        if (element.type != resp::Type::bulk_string) {
            return false;
        }
        arguments.push_back(element.text);
    }
    return true;
}

[[nodiscard]] bool contains_error(const resp::Value& value, std::string& message) {
    if (value.type == resp::Type::error) {
        message = value.text;
        return true;
    }
    if (value.type == resp::Type::array) {
        for (const auto& element : value.elements) {
            if (contains_error(element, message)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

class Server::Impl {
public:
    explicit Impl(Config config) : config_(std::move(config)) {}

    [[nodiscard]] bool initialize(std::string& error) {
        sigset_t mask{};
        if (::sigemptyset(&mask) < 0 || ::sigaddset(&mask, SIGINT) < 0 ||
            ::sigaddset(&mask, SIGTERM) < 0 || ::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
            error = system_error("cannot block shutdown signals");
            return false;
        }
        signal_fd_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
        if (!signal_fd_) {
            error = system_error("cannot create signal fd");
            return false;
        }

        if (config_.append_only) {
            persistence::FsyncPolicy policy = persistence::FsyncPolicy::everysec;
            if (config_.append_fsync == AppendFsync::always) {
                policy = persistence::FsyncPolicy::always;
            } else if (config_.append_fsync == AppendFsync::no) {
                policy = persistence::FsyncPolicy::no;
            }
            aof_ = std::make_unique<persistence::Aof>(config_.data_dir / "appendonly.aof", policy,
                                                      config_.protocol_limits);
            if (!aof_->open(error)) {
                return false;
            }
            commands::Session replay_session;
            engine_.begin_replay();
            const bool replayed = aof_->replay(
                [this, &replay_session](const std::vector<std::string>& command,
                                        std::string& replay_error) {
                    const auto result = engine_.execute_replay(replay_session, command);
                    return !contains_error(result.reply, replay_error);
                },
                error);
            engine_.finish_replay();
            if (!replayed) {
                return false;
            }
            engine_.set_persistence(aof_.get());
            aof_->start_background_flush();
        }

        listener_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (!listener_) {
            error = system_error("cannot create listener socket");
            return false;
        }
        if (!make_nonblocking_and_cloexec(listener_.get(), error)) {
            return false;
        }
        int enabled = 1;
        if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
            error = system_error("cannot set SO_REUSEADDR");
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);
        if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
            error = "invalid IPv4 bind address: " + config_.bind_address;
            return false;
        }
        if (::bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            error = system_error("cannot bind listener");
            return false;
        }
        const auto backlog = static_cast<int>(
            std::min(config_.max_clients, static_cast<std::size_t>(SOMAXCONN)));
        if (::listen(listener_.get(), backlog) < 0) {
            error = system_error("cannot listen");
            return false;
        }

        epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
        if (!epoll_fd_) {
            error = system_error("cannot create epoll instance");
            return false;
        }
        if (!add_epoll(listener_.get(), listener_token, EPOLLIN, error) ||
            !add_epoll(signal_fd_.get(), signal_token, EPOLLIN, error)) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    [[nodiscard]] int run() {
        if (!initialized_) {
            std::cerr << "KeyForge internal error: server was not initialized\n";
            return 1;
        }
        std::cout << "KeyForge listening on " << config_.bind_address << ':' << config_.port
                  << " (RESP2, appendonly " << (config_.append_only ? "yes" : "no") << ")\n";
        std::cout.flush();

        std::vector<epoll_event> events(static_cast<std::size_t>(max_events));
        while (!stopping_) {
            (void)engine_.active_expire(64U);
            const int count = ::epoll_wait(epoll_fd_.get(), events.data(), max_events, 100);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << system_error("epoll_wait failed") << '\n';
                return 1;
            }
            for (int index = 0; index < count && !stopping_; ++index) {
                const auto token = events[static_cast<std::size_t>(index)].data.u64;
                const auto flags = events[static_cast<std::size_t>(index)].events;
                try {
                    if (token == listener_token) {
                        accept_ready();
                    } else if (token == signal_token) {
                        consume_shutdown_signal();
                    } else {
                        client_ready(token, flags);
                    }
                } catch (const std::exception& exception) {
                    std::cerr << "connection handler exception: " << exception.what() << '\n';
                    if (token > signal_token) {
                        remove_client(token);
                    }
                } catch (...) {
                    std::cerr << "connection handler exception: unknown error\n";
                    if (token > signal_token) {
                        remove_client(token);
                    }
                }
            }
        }
        clients_.clear();
        if (aof_ != nullptr) {
            std::string error;
            if (!aof_->flush(error)) {
                std::cerr << "AOF shutdown flush failed: " << error << '\n';
                return 1;
            }
        }
        std::cout << "KeyForge stopped cleanly\n";
        return 0;
    }

private:
    struct Client {
        Client(Fd descriptor, resp::Limits limits)
            : socket(std::move(descriptor)), parser(limits) {}

        Fd socket;
        resp::Parser parser;
        commands::Session session;
        std::string output;
        std::size_t output_offset{0};
        bool close_after_write{false};
    };

    [[nodiscard]] bool add_epoll(int fd, std::uint64_t token, std::uint32_t events,
                                 std::string& error) const {
        epoll_event event{};
        event.events = events;
        event.data.u64 = token;
        if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event) < 0) {
            error = system_error("epoll add failed");
            return false;
        }
        return true;
    }

    void update_interest(std::uint64_t token, Client& client) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        if (client.output_offset < client.output.size()) {
            event.events |= EPOLLOUT;
        }
        event.data.u64 = token;
        if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, client.socket.get(), &event) < 0) {
            throw std::runtime_error(system_error("epoll modify failed"));
        }
    }

    void accept_ready() {
        for (;;) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const int descriptor = ::accept4(listener_.get(), reinterpret_cast<sockaddr*>(&peer),
                                             &peer_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (descriptor < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == EINTR || errno == ECONNABORTED) {
                    continue;
                }
                std::cerr << system_error("accept failed") << '\n';
                return;
            }
            Fd socket{descriptor};
            if (clients_.size() >= config_.max_clients) {
                const std::string rejection = resp::encode(resp::Value::error("ERR max clients reached"));
                (void)::send(socket.get(), rejection.data(), rejection.size(), MSG_NOSIGNAL);
                continue;
            }
            const std::uint64_t token = next_client_token_++;
            std::string error;
            if (!add_epoll(socket.get(), token, EPOLLIN | EPOLLRDHUP, error)) {
                std::cerr << error << '\n';
                continue;
            }
            clients_.emplace(token, Client{std::move(socket), config_.protocol_limits});
        }
    }

    void consume_shutdown_signal() {
        signalfd_siginfo information{};
        for (;;) {
            const ssize_t count = ::read(signal_fd_.get(), &information, sizeof(information));
            if (count == static_cast<ssize_t>(sizeof(information))) {
                stopping_ = true;
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
    }

    void client_ready(std::uint64_t token, std::uint32_t flags) {
        auto iterator = clients_.find(token);
        if (iterator == clients_.end()) {
            return;
        }
        if ((flags & (EPOLLERR | EPOLLHUP)) != 0U) {
            remove_client(token);
            return;
        }
        bool alive = true;
        if ((flags & EPOLLIN) != 0U) {
            alive = read_client(token, iterator->second);
        }
        if (!alive) {
            remove_client(token);
            return;
        }
        iterator = clients_.find(token);
        if (iterator == clients_.end()) {
            return;
        }
        if ((flags & EPOLLOUT) != 0U || iterator->second.output_offset < iterator->second.output.size()) {
            alive = write_client(iterator->second);
        }
        if (!alive) {
            remove_client(token);
            return;
        }
        update_interest(token, iterator->second);
        if ((flags & EPOLLRDHUP) != 0U &&
            iterator->second.output_offset == iterator->second.output.size()) {
            remove_client(token);
        }
    }

    [[nodiscard]] bool read_client(std::uint64_t token, Client& client) {
        char buffer[16384];
        for (;;) {
            const ssize_t count = ::recv(client.socket.get(), buffer, sizeof(buffer), 0);
            if (count > 0) {
                if (!client.parser.feed(
                        std::string_view{buffer, static_cast<std::size_t>(count)})) {
                    (void)queue_reply(client,
                                      resp::Value::error("ERR " +
                                                         std::string{client.parser.error()}));
                    client.close_after_write = true;
                    return true;
                }
                if (!process_frames(token, client)) {
                    return false;
                }
                continue;
            }
            if (count == 0) {
                if (client.output_offset < client.output.size()) {
                    client.close_after_write = true;
                    return true;
                }
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
    }

    [[nodiscard]] bool process_frames(std::uint64_t token, Client& client) {
        (void)token;
        std::vector<std::string> arguments;
        for (;;) {
            auto parsed = client.parser.next();
            if (parsed.status == resp::ParseStatus::incomplete) {
                return true;
            }
            if (parsed.status == resp::ParseStatus::invalid) {
                (void)queue_reply(client,
                                  resp::Value::error("ERR Protocol error: " + parsed.error));
                client.close_after_write = true;
                return true;
            }
            if (!parsed.value.has_value() || !command_arguments(*parsed.value, arguments)) {
                (void)queue_reply(
                    client, resp::Value::error(
                                "ERR Protocol error: expected non-empty array of bulk strings"));
                client.close_after_write = true;
                return true;
            }
            auto result = engine_.execute(token, client.session, arguments);
            if (!queue_reply(client, result.reply)) {
                return false;
            }
            bool current_client_alive = true;
            for (auto& delivery : result.deliveries) {
                auto recipient = clients_.find(delivery.connection_id);
                if (recipient == clients_.end()) {
                    continue;
                }
                if (!queue_reply(recipient->second, delivery.message)) {
                    if (delivery.connection_id == token) {
                        current_client_alive = false;
                    } else {
                        remove_client(delivery.connection_id);
                    }
                    continue;
                }
                if (delivery.connection_id != token) {
                    update_interest(delivery.connection_id, recipient->second);
                }
            }
            if (!current_client_alive) {
                return false;
            }
            if (result.close_after_reply) {
                client.close_after_write = true;
                return true;
            }
        }
    }

    [[nodiscard]] bool queue_reply(Client& client, const resp::Value& value) {
        const std::string wire = resp::encode(value);
        const std::size_t pending = client.output.size() - client.output_offset;
        if (wire.size() > config_.max_client_output_bytes ||
            pending > config_.max_client_output_bytes - wire.size()) {
            return false;
        }
        if (client.output_offset > 0U) {
            client.output.erase(0, client.output_offset);
            client.output_offset = 0;
        }
        client.output.append(wire);
        return true;
    }

    [[nodiscard]] bool write_client(Client& client) {
        while (client.output_offset < client.output.size()) {
            const char* const data = client.output.data() + client.output_offset;
            const std::size_t remaining = client.output.size() - client.output_offset;
            const ssize_t count =
                ::send(client.socket.get(), data, remaining, MSG_NOSIGNAL);
            if (count > 0) {
                client.output_offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            return false;
        }
        client.output.clear();
        client.output_offset = 0;
        return !client.close_after_write;
    }

    void remove_client(std::uint64_t token) {
        const auto iterator = clients_.find(token);
        if (iterator != clients_.end()) {
            engine_.disconnect(token, iterator->second.session);
            clients_.erase(iterator);
        }
    }

    Config config_;
    SystemClock clock_;
    commands::Engine engine_{clock_};
    std::unique_ptr<persistence::Aof> aof_;
    Fd listener_;
    Fd epoll_fd_;
    Fd signal_fd_;
    std::unordered_map<std::uint64_t, Client> clients_;
    std::uint64_t next_client_token_{3U};
    bool initialized_{false};
    bool stopping_{false};
};

Server::Server(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Server::~Server() = default;

bool Server::initialize(std::string& error) { return impl_->initialize(error); }

int Server::run() { return impl_->run(); }

}  // namespace keyforge::server

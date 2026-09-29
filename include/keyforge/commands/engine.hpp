#pragma once

#include "keyforge/common/clock.hpp"
#include "keyforge/protocol/resp.hpp"
#include "keyforge/storage/database.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace keyforge::persistence {
class Sink;
}

namespace keyforge::commands {

struct Session {
    bool in_transaction{false};
    bool transaction_failed{false};
    std::vector<std::vector<std::string>> queued_commands;
    std::unordered_map<std::string, std::uint64_t> watched_versions;
};

struct Delivery {
    std::uint64_t connection_id{0};
    resp::Value message;
};

struct Result {
    resp::Value reply{resp::Value::error("ERR internal error")};
    bool close_after_reply{false};
    std::vector<Delivery> deliveries;

    Result() = default;
    Result(resp::Value reply_value, bool close = false,
           std::vector<Delivery> pending_deliveries = {})
        : reply(std::move(reply_value)), close_after_reply(close),
          deliveries(std::move(pending_deliveries)) {}
};

class Engine {
public:
    explicit Engine(Clock& clock);

    [[nodiscard]] Result execute(std::uint64_t connection_id, Session& session,
                                 const std::vector<std::string>& arguments);
    [[nodiscard]] Result execute_replay(Session& session,
                                        const std::vector<std::string>& arguments);
    void set_persistence(persistence::Sink* sink) noexcept;
    void begin_replay() noexcept;
    void finish_replay();
    void disconnect(std::uint64_t connection_id, Session& session);
    [[nodiscard]] std::size_t active_expire(std::size_t work_limit);

    [[nodiscard]] storage::Database& database() noexcept;
    [[nodiscard]] const storage::Database& database() const noexcept;
    [[nodiscard]] std::uint64_t commands_processed() const noexcept;

private:
    [[nodiscard]] Result execute_direct(std::uint64_t connection_id, Session& session,
                                        const std::vector<std::string>& arguments);
    [[nodiscard]] Result execute_internal(std::uint64_t connection_id, Session& session,
                                          const std::vector<std::string>& arguments,
                                          bool persist);

    storage::Database database_;
    std::uint64_t commands_processed_{0};
    persistence::Sink* persistence_{nullptr};
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> channel_subscribers_;
    std::unordered_map<std::uint64_t, std::unordered_set<std::string>> connection_channels_;
};

}  // namespace keyforge::commands

#pragma once

#include "keyforge/common/clock.hpp"
#include "keyforge/graph/dependency_graph.hpp"
#include "keyforge/storage/sorted_set.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace keyforge::storage {

using String = std::string;
using List = std::deque<std::string>;
using Hash = std::unordered_map<std::string, std::string>;
using Set = std::unordered_set<std::string>;
using Data = std::variant<String, List, Hash, Set, SortedSet>;

struct Entry {
    Data data;
    std::optional<std::int64_t> expires_at_unix_ms;
    std::optional<std::int64_t> expires_at_monotonic_ms;
    std::uint64_t expiry_token{0};
};

class Database {
public:
    explicit Database(Clock& clock);

    [[nodiscard]] Entry* find(std::string_view key);
    [[nodiscard]] bool exists(std::string_view key);
    [[nodiscard]] Entry& create(std::string key, Data data);
    void replace(std::string key, Data data);
    void touch(std::string_view key);
    [[nodiscard]] bool erase(std::string_view key);
    [[nodiscard]] std::size_t erase_many(const std::vector<std::string>& keys);
    [[nodiscard]] std::size_t invalidate(std::string_view key);
    void clear();

    [[nodiscard]] bool set_expiration(std::string_view key, std::int64_t deadline_ms);
    [[nodiscard]] bool persist(std::string_view key);
    [[nodiscard]] std::optional<std::int64_t> expiration(std::string_view key);
    [[nodiscard]] std::optional<std::int64_t> remaining_ttl(std::string_view key);
    [[nodiscard]] std::size_t active_expire(std::size_t work_limit);

    [[nodiscard]] std::uint64_t version(std::string_view key);
    [[nodiscard]] std::size_t size();
    [[nodiscard]] std::vector<std::string> keys();

    [[nodiscard]] graph::DependencyGraph& dependencies() noexcept;
    [[nodiscard]] const graph::DependencyGraph& dependencies() const noexcept;
    [[nodiscard]] Clock& clock() noexcept;
    void pause_expiration(bool paused) noexcept;

private:
    struct ExpirationRecord {
        std::int64_t monotonic_deadline_ms{0};
        std::string key;
        std::uint64_t token{0};
    };

    struct ExpirationLater {
        [[nodiscard]] bool operator()(const ExpirationRecord& left,
                                      const ExpirationRecord& right) const noexcept;
    };

    [[nodiscard]] bool expire_if_needed(std::string_view key);
    void bump_version(std::string_view key);

    Clock& clock_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, std::uint64_t> versions_;
    std::priority_queue<ExpirationRecord, std::vector<ExpirationRecord>, ExpirationLater>
        expirations_;
    graph::DependencyGraph dependencies_;
    std::uint64_t next_expiry_token_{1U};
    bool expiration_paused_{false};
};

[[nodiscard]] std::string_view type_name(const Entry& entry) noexcept;

}  // namespace keyforge::storage

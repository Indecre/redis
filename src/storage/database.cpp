#include "keyforge/storage/database.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace keyforge::storage {

Database::Database(Clock& clock) : clock_(clock) {}

Entry* Database::find(std::string_view key) {
    if (expire_if_needed(key)) {
        return nullptr;
    }
    const auto iterator = entries_.find(std::string{key});
    return iterator == entries_.end() ? nullptr : &iterator->second;
}

bool Database::exists(std::string_view key) { return find(key) != nullptr; }

Entry& Database::create(std::string key, Data data) {
    const auto [iterator, inserted] =
        entries_.emplace(std::move(key),
                         Entry{std::move(data), std::nullopt, std::nullopt, 0});
    (void)inserted;
    bump_version(iterator->first);
    return iterator->second;
}

void Database::replace(std::string key, Data data) {
    const auto existing = entries_.find(key);
    if (existing == entries_.end()) {
        entries_.emplace(key, Entry{std::move(data), std::nullopt, std::nullopt, 0});
    } else {
        existing->second.data = std::move(data);
        existing->second.expires_at_unix_ms.reset();
        existing->second.expires_at_monotonic_ms.reset();
        existing->second.expiry_token = 0;
    }
    bump_version(key);
}

void Database::touch(std::string_view key) { bump_version(key); }

bool Database::erase(std::string_view key) {
    const std::string owned_key{key};
    const auto erased = entries_.erase(owned_key);
    if (erased == 0U) {
        return false;
    }
    dependencies_.remove_key(owned_key);
    bump_version(owned_key);
    return true;
}

std::size_t Database::erase_many(const std::vector<std::string>& keys_value) {
    std::size_t count = 0;
    for (const auto& key : keys_value) {
        if (find(key) != nullptr && erase(key)) {
            ++count;
        }
    }
    return count;
}

std::size_t Database::invalidate(std::string_view key) {
    const auto cascade_keys = dependencies_.cascade(key);
    std::size_t count = 0;
    for (const auto& cascade_key : cascade_keys) {
        if (entries_.erase(cascade_key) != 0U) {
            bump_version(cascade_key);
            ++count;
        }
    }
    for (const auto& cascade_key : cascade_keys) {
        dependencies_.remove_key(cascade_key);
    }
    return count;
}

void Database::clear() {
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        bump_version(key);
    }
    entries_.clear();
    dependencies_.clear();
    expirations_ = {};
}

bool Database::set_expiration(std::string_view key, std::int64_t deadline_ms) {
    Entry* const entry = find(key);
    if (entry == nullptr) {
        return false;
    }
    const std::string owned_key{key};
    const auto unix_now = clock_.unix_milliseconds();
    const auto monotonic_now = clock_.monotonic_milliseconds();
    std::int64_t monotonic_deadline = monotonic_now;
    if (deadline_ms > unix_now) {
        const auto remaining =
            unix_now < 0 && deadline_ms > std::numeric_limits<std::int64_t>::max() + unix_now
                ? std::numeric_limits<std::int64_t>::max()
                : deadline_ms - unix_now;
        monotonic_deadline =
            monotonic_now > std::numeric_limits<std::int64_t>::max() - remaining
                ? std::numeric_limits<std::int64_t>::max()
                : monotonic_now + remaining;
    }
    entry->expires_at_unix_ms = deadline_ms;
    entry->expires_at_monotonic_ms = monotonic_deadline;
    entry->expiry_token = next_expiry_token_++;
    expirations_.push({monotonic_deadline, owned_key, entry->expiry_token});
    bump_version(owned_key);
    if (!expiration_paused_) {
        (void)expire_if_needed(owned_key);
    }
    return true;
}

bool Database::persist(std::string_view key) {
    Entry* const entry = find(key);
    if (entry == nullptr || !entry->expires_at_unix_ms.has_value()) {
        return false;
    }
    entry->expires_at_unix_ms.reset();
    entry->expires_at_monotonic_ms.reset();
    entry->expiry_token = 0;
    bump_version(key);
    return true;
}

std::optional<std::int64_t> Database::expiration(std::string_view key) {
    Entry* const entry = find(key);
    return entry == nullptr ? std::nullopt : entry->expires_at_unix_ms;
}

std::optional<std::int64_t> Database::remaining_ttl(std::string_view key) {
    Entry* const entry = find(key);
    if (entry == nullptr || !entry->expires_at_monotonic_ms.has_value()) {
        return std::nullopt;
    }
    const auto now = clock_.monotonic_milliseconds();
    if (*entry->expires_at_monotonic_ms <= now) {
        return 0;
    }
    if (now < 0 && *entry->expires_at_monotonic_ms >
                       std::numeric_limits<std::int64_t>::max() + now) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return *entry->expires_at_monotonic_ms - now;
}

std::size_t Database::active_expire(std::size_t work_limit) {
    std::size_t expired = 0;
    std::size_t work = 0;
    const auto now = clock_.monotonic_milliseconds();
    while (work < work_limit && !expirations_.empty()) {
        const auto record = expirations_.top();
        if (record.monotonic_deadline_ms > now) {
            break;
        }
        expirations_.pop();
        ++work;
        const auto iterator = entries_.find(record.key);
        if (iterator == entries_.end() || iterator->second.expiry_token != record.token ||
            !iterator->second.expires_at_monotonic_ms.has_value() ||
            *iterator->second.expires_at_monotonic_ms > now) {
            continue;
        }
        expired += invalidate(record.key);
    }
    return expired;
}

std::uint64_t Database::version(std::string_view key) {
    (void)find(key);
    const auto iterator = versions_.find(std::string{key});
    return iterator == versions_.end() ? 0U : iterator->second;
}

std::size_t Database::size() {
    (void)active_expire(std::numeric_limits<std::size_t>::max());
    return entries_.size();
}

std::vector<std::string> Database::keys() {
    (void)active_expire(std::numeric_limits<std::size_t>::max());
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        result.push_back(key);
    }
    std::sort(result.begin(), result.end());
    return result;
}

graph::DependencyGraph& Database::dependencies() noexcept { return dependencies_; }

const graph::DependencyGraph& Database::dependencies() const noexcept { return dependencies_; }

Clock& Database::clock() noexcept { return clock_; }

void Database::pause_expiration(bool paused) noexcept { expiration_paused_ = paused; }

bool Database::ExpirationLater::operator()(const ExpirationRecord& left,
                                           const ExpirationRecord& right) const noexcept {
    return left.monotonic_deadline_ms > right.monotonic_deadline_ms;
}

bool Database::expire_if_needed(std::string_view key) {
    if (expiration_paused_) {
        return false;
    }
    const auto iterator = entries_.find(std::string{key});
    if (iterator == entries_.end() || !iterator->second.expires_at_monotonic_ms.has_value() ||
        *iterator->second.expires_at_monotonic_ms > clock_.monotonic_milliseconds()) {
        return false;
    }
    (void)invalidate(key);
    return true;
}

void Database::bump_version(std::string_view key) {
    auto& value = versions_[std::string{key}];
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

std::string_view type_name(const Entry& entry) noexcept {
    if (std::holds_alternative<String>(entry.data)) {
        return "string";
    }
    if (std::holds_alternative<List>(entry.data)) {
        return "list";
    }
    if (std::holds_alternative<Hash>(entry.data)) {
        return "hash";
    }
    if (std::holds_alternative<Set>(entry.data)) {
        return "set";
    }
    return "zset";
}

}  // namespace keyforge::storage

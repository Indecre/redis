#include "keyforge/commands/engine.hpp"

#include "keyforge/common/version.hpp"
#include "keyforge/persistence/aof.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace keyforge::commands {
namespace {

enum class CommandId {
    ping,
    echo,
    quit,
    info,
    del,
    exists,
    type,
    dbsize,
    flushdb,
    keys,
    set,
    get,
    mget,
    mset,
    incr,
    decr,
    incrby,
    append,
    strlen,
    expire,
    pexpire,
    pexpireat,
    ttl,
    pttl,
    persist,
    lpush,
    rpush,
    lpop,
    rpop,
    llen,
    lrange,
    hset,
    hget,
    hmget,
    hgetall,
    hdel,
    hexists,
    hlen,
    sadd,
    srem,
    sismember,
    smembers,
    scard,
    zadd,
    zrem,
    zscore,
    zcard,
    zrange,
    multi,
    exec,
    discard,
    watch,
    unwatch,
    publish,
    subscribe,
    unsubscribe,
    depend,
    deps,
    dependents,
    invalidate,
};

struct CommandSpec {
    std::string_view name;
    CommandId id;
    std::size_t minimum_arguments;
    std::size_t maximum_arguments;
    bool mutating;
    bool transaction_control;
    bool pubsub_safe;
};

constexpr std::array command_specs{
    CommandSpec{"PING", CommandId::ping, 1, 2, false, false, true},
    CommandSpec{"ECHO", CommandId::echo, 2, 2, false, false, false},
    CommandSpec{"QUIT", CommandId::quit, 1, 1, false, false, true},
    CommandSpec{"INFO", CommandId::info, 1, 1, false, false, false},
    CommandSpec{"DEL", CommandId::del, 2, 0, true, false, false},
    CommandSpec{"EXISTS", CommandId::exists, 2, 0, false, false, false},
    CommandSpec{"TYPE", CommandId::type, 2, 2, false, false, false},
    CommandSpec{"DBSIZE", CommandId::dbsize, 1, 1, false, false, false},
    CommandSpec{"FLUSHDB", CommandId::flushdb, 1, 1, true, false, false},
    CommandSpec{"KEYS", CommandId::keys, 2, 2, false, false, false},
    CommandSpec{"SET", CommandId::set, 3, 0, true, false, false},
    CommandSpec{"GET", CommandId::get, 2, 2, false, false, false},
    CommandSpec{"MGET", CommandId::mget, 2, 0, false, false, false},
    CommandSpec{"MSET", CommandId::mset, 3, 0, true, false, false},
    CommandSpec{"INCR", CommandId::incr, 2, 2, true, false, false},
    CommandSpec{"DECR", CommandId::decr, 2, 2, true, false, false},
    CommandSpec{"INCRBY", CommandId::incrby, 3, 3, true, false, false},
    CommandSpec{"APPEND", CommandId::append, 3, 3, true, false, false},
    CommandSpec{"STRLEN", CommandId::strlen, 2, 2, false, false, false},
    CommandSpec{"EXPIRE", CommandId::expire, 3, 3, true, false, false},
    CommandSpec{"PEXPIRE", CommandId::pexpire, 3, 3, true, false, false},
    CommandSpec{"PEXPIREAT", CommandId::pexpireat, 3, 3, true, false, false},
    CommandSpec{"TTL", CommandId::ttl, 2, 2, false, false, false},
    CommandSpec{"PTTL", CommandId::pttl, 2, 2, false, false, false},
    CommandSpec{"PERSIST", CommandId::persist, 2, 2, true, false, false},
    CommandSpec{"LPUSH", CommandId::lpush, 3, 0, true, false, false},
    CommandSpec{"RPUSH", CommandId::rpush, 3, 0, true, false, false},
    CommandSpec{"LPOP", CommandId::lpop, 2, 2, true, false, false},
    CommandSpec{"RPOP", CommandId::rpop, 2, 2, true, false, false},
    CommandSpec{"LLEN", CommandId::llen, 2, 2, false, false, false},
    CommandSpec{"LRANGE", CommandId::lrange, 4, 4, false, false, false},
    CommandSpec{"HSET", CommandId::hset, 4, 0, true, false, false},
    CommandSpec{"HGET", CommandId::hget, 3, 3, false, false, false},
    CommandSpec{"HMGET", CommandId::hmget, 3, 0, false, false, false},
    CommandSpec{"HGETALL", CommandId::hgetall, 2, 2, false, false, false},
    CommandSpec{"HDEL", CommandId::hdel, 3, 0, true, false, false},
    CommandSpec{"HEXISTS", CommandId::hexists, 3, 3, false, false, false},
    CommandSpec{"HLEN", CommandId::hlen, 2, 2, false, false, false},
    CommandSpec{"SADD", CommandId::sadd, 3, 0, true, false, false},
    CommandSpec{"SREM", CommandId::srem, 3, 0, true, false, false},
    CommandSpec{"SISMEMBER", CommandId::sismember, 3, 3, false, false, false},
    CommandSpec{"SMEMBERS", CommandId::smembers, 2, 2, false, false, false},
    CommandSpec{"SCARD", CommandId::scard, 2, 2, false, false, false},
    CommandSpec{"ZADD", CommandId::zadd, 4, 0, true, false, false},
    CommandSpec{"ZREM", CommandId::zrem, 3, 0, true, false, false},
    CommandSpec{"ZSCORE", CommandId::zscore, 3, 3, false, false, false},
    CommandSpec{"ZCARD", CommandId::zcard, 2, 2, false, false, false},
    CommandSpec{"ZRANGE", CommandId::zrange, 4, 5, false, false, false},
    CommandSpec{"MULTI", CommandId::multi, 1, 1, false, true, false},
    CommandSpec{"EXEC", CommandId::exec, 1, 1, false, true, false},
    CommandSpec{"DISCARD", CommandId::discard, 1, 1, false, true, false},
    CommandSpec{"WATCH", CommandId::watch, 2, 0, false, true, false},
    CommandSpec{"UNWATCH", CommandId::unwatch, 1, 1, false, true, false},
    CommandSpec{"PUBLISH", CommandId::publish, 3, 3, false, false, true},
    CommandSpec{"SUBSCRIBE", CommandId::subscribe, 2, 0, false, true, true},
    CommandSpec{"UNSUBSCRIBE", CommandId::unsubscribe, 1, 0, false, true, true},
    CommandSpec{"KFG.DEPEND", CommandId::depend, 3, 0, true, false, false},
    CommandSpec{"KFG.DEPS", CommandId::deps, 2, 2, false, false, false},
    CommandSpec{"KFG.DEPENDENTS", CommandId::dependents, 2, 2, false, false, false},
    CommandSpec{"KFG.INVALIDATE", CommandId::invalidate, 2, 2, true, false, false},
};

[[nodiscard]] std::string ascii_upper(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (const unsigned char character : input) {
        if (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) {
            result.push_back(static_cast<char>(character - static_cast<unsigned char>('a') +
                                               static_cast<unsigned char>('A')));
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

[[nodiscard]] const CommandSpec* find_spec(std::string_view name) {
    const std::string upper = ascii_upper(name);
    const auto iterator = std::find_if(command_specs.begin(), command_specs.end(),
                                       [&upper](const CommandSpec& spec) {
                                           return spec.name == upper;
                                       });
    return iterator == command_specs.end() ? nullptr : &*iterator;
}

[[nodiscard]] resp::Value wrong_arity(std::string_view command) {
    return resp::Value::error("ERR wrong number of arguments for '" + ascii_upper(command) +
                              "' command");
}

[[nodiscard]] resp::Value wrong_type() {
    return resp::Value::error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

[[nodiscard]] bool valid_arity(const CommandSpec& spec, std::size_t count) {
    return count >= spec.minimum_arguments &&
           (spec.maximum_arguments == 0U || count <= spec.maximum_arguments);
}

[[nodiscard]] bool parse_i64(std::string_view text, std::int64_t& output) {
    if (text.empty() || text.front() == '+') {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_double(std::string_view text, double& output) {
    if (text.empty()) {
        return false;
    }
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), output, std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           !std::isnan(output);
}

[[nodiscard]] std::string format_double(double value) {
    char buffer[128]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        return "0";
    }
    return std::string{buffer, result.ptr};
}

[[nodiscard]] bool checked_add(std::int64_t left, std::int64_t right,
                               std::int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(std::int64_t value, std::int64_t factor,
                                    std::int64_t& result) {
    if (value > std::numeric_limits<std::int64_t>::max() / factor ||
        value < std::numeric_limits<std::int64_t>::min() / factor) {
        return false;
    }
    result = value * factor;
    return true;
}

[[nodiscard]] bool glob_matches(std::string_view pattern, std::string_view value) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' || pattern[pattern_index] == value[value_index])) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        } else if (star != std::string_view::npos) {
            pattern_index = star + 1U;
            value_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

struct IndexRange {
    std::size_t begin{0};
    std::size_t end{0};
    bool empty{true};
};

[[nodiscard]] IndexRange normalize_range(std::int64_t start, std::int64_t stop,
                                         std::size_t size) {
    if (size == 0U) {
        return {};
    }
    const auto count = static_cast<std::int64_t>(
        std::min(size, static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
    if (start < 0) {
        start = std::max<std::int64_t>(0, count + start);
    }
    if (stop < 0) {
        stop = count + stop;
    }
    if (start >= count || stop < 0 || start > stop) {
        return {};
    }
    stop = std::min(stop, count - 1);
    return {static_cast<std::size_t>(start), static_cast<std::size_t>(stop), false};
}

[[nodiscard]] std::optional<std::string> validate_syntax(
    CommandId id, const std::vector<std::string>& arguments) {
    if ((id == CommandId::mset && arguments.size() % 2U == 0U) ||
        (id == CommandId::hset && arguments.size() % 2U != 0U) ||
        (id == CommandId::zadd && arguments.size() % 2U != 0U)) {
        return "ERR syntax error";
    }
    if (id == CommandId::set) {
        bool condition = false;
        bool expiration = false;
        for (std::size_t index = 3; index < arguments.size(); ++index) {
            const std::string option = ascii_upper(arguments[index]);
            if (option == "NX" || option == "XX") {
                if (condition) {
                    return "ERR syntax error";
                }
                condition = true;
            } else if (option == "EX" || option == "PX") {
                if (expiration || index + 1U >= arguments.size()) {
                    return "ERR syntax error";
                }
                std::int64_t amount = 0;
                if (!parse_i64(arguments[++index], amount) || amount <= 0) {
                    return "ERR invalid expire time in 'set' command";
                }
                expiration = true;
            } else {
                return "ERR syntax error";
            }
        }
    }
    if (id == CommandId::incrby || id == CommandId::expire || id == CommandId::pexpire ||
        id == CommandId::pexpireat || id == CommandId::lrange) {
        const std::size_t first_numeric = id == CommandId::lrange ? 2U : 2U;
        const std::size_t last_numeric = id == CommandId::lrange ? 3U : 2U;
        for (std::size_t index = first_numeric; index <= last_numeric; ++index) {
            std::int64_t ignored = 0;
            if (!parse_i64(arguments[index], ignored)) {
                return "ERR value is not an integer or out of range";
            }
        }
    }
    if (id == CommandId::zadd) {
        for (std::size_t index = 2; index < arguments.size(); index += 2U) {
            double ignored = 0;
            if (!parse_double(arguments[index], ignored)) {
                return "ERR value is not a valid float";
            }
        }
    }
    if (id == CommandId::zrange && arguments.size() == 5U &&
        ascii_upper(arguments[4]) != "WITHSCORES") {
        return "ERR syntax error";
    }
    return std::nullopt;
}

[[nodiscard]] resp::Value bulk_array(const std::vector<std::string>& values) {
    std::vector<resp::Value> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(resp::Value::bulk(value));
    }
    return resp::Value::array(std::move(result));
}

[[nodiscard]] std::vector<std::vector<std::string>> canonical_commands(
    const CommandSpec& spec, const std::vector<std::string>& arguments,
    const Result& result, storage::Database& database) {
    if (!spec.mutating || result.reply.type == resp::Type::error ||
        result.reply.type == resp::Type::null_bulk_string) {
        return {};
    }
    if (spec.id == CommandId::set) {
        std::vector<std::vector<std::string>> commands{{"SET", arguments[1], arguments[2]}};
        const auto expiry = database.expiration(arguments[1]);
        if (expiry.has_value()) {
            commands.push_back({"PEXPIREAT", arguments[1], std::to_string(*expiry)});
        }
        return commands;
    }
    if (spec.id == CommandId::expire || spec.id == CommandId::pexpire ||
        spec.id == CommandId::pexpireat) {
        if (result.reply.type != resp::Type::integer || result.reply.integer == 0) {
            return {};
        }
        const auto expiry = database.expiration(arguments[1]);
        return expiry.has_value()
                   ? std::vector<std::vector<std::string>>{
                         {"PEXPIREAT", arguments[1], std::to_string(*expiry)}}
                   : std::vector<std::vector<std::string>>{};
    }
    if (spec.id == CommandId::persist && result.reply.type == resp::Type::integer &&
        result.reply.integer == 0) {
        return {};
    }
    return {arguments};
}

}  // namespace

Engine::Engine(Clock& clock) : database_(clock) {}

Result Engine::execute(std::uint64_t connection_id, Session& session,
                       const std::vector<std::string>& arguments) {
    return execute_internal(connection_id, session, arguments, true);
}

Result Engine::execute_replay(Session& session, const std::vector<std::string>& arguments) {
    return execute_internal(0, session, arguments, false);
}

void Engine::set_persistence(persistence::Sink* sink) noexcept { persistence_ = sink; }

void Engine::begin_replay() noexcept { database_.pause_expiration(true); }

void Engine::finish_replay() {
    database_.pause_expiration(false);
    (void)database_.active_expire(std::numeric_limits<std::size_t>::max());
}

Result Engine::execute_internal(std::uint64_t connection_id, Session& session,
                                const std::vector<std::string>& arguments, bool persist) {
    ++commands_processed_;
    if (arguments.empty()) {
        return {resp::Value::error("ERR empty command")};
    }
    const CommandSpec* const spec = find_spec(arguments.front());
    if (spec == nullptr) {
        if (session.in_transaction) {
            session.transaction_failed = true;
        }
        return {resp::Value::error("ERR unknown command '" + arguments.front() + "'")};
    }
    if (!valid_arity(*spec, arguments.size())) {
        if (session.in_transaction) {
            session.transaction_failed = true;
        }
        return {wrong_arity(arguments.front())};
    }

    const bool subscribed = connection_channels_.contains(connection_id) &&
                            !connection_channels_.at(connection_id).empty();
    if (subscribed && !spec->pubsub_safe) {
        return {resp::Value::error(
            "ERR only (P)SUBSCRIBE / (P)UNSUBSCRIBE / PING / QUIT / PUBLISH are allowed in this context")};
    }

    if (spec->id == CommandId::multi) {
        if (session.in_transaction) {
            return {resp::Value::error("ERR MULTI calls can not be nested")};
        }
        session.in_transaction = true;
        session.transaction_failed = false;
        session.queued_commands.clear();
        return {resp::Value::simple("OK")};
    }
    if (spec->id == CommandId::discard) {
        if (!session.in_transaction) {
            return {resp::Value::error("ERR DISCARD without MULTI")};
        }
        session.in_transaction = false;
        session.transaction_failed = false;
        session.queued_commands.clear();
        session.watched_versions.clear();
        return {resp::Value::simple("OK")};
    }
    if (spec->id == CommandId::watch) {
        if (session.in_transaction) {
            return {resp::Value::error("ERR WATCH inside MULTI is not allowed")};
        }
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            session.watched_versions[arguments[index]] = database_.version(arguments[index]);
        }
        return {resp::Value::simple("OK")};
    }
    if (spec->id == CommandId::unwatch) {
        session.watched_versions.clear();
        return {resp::Value::simple("OK")};
    }
    if (spec->id == CommandId::exec) {
        if (!session.in_transaction) {
            return {resp::Value::error("ERR EXEC without MULTI")};
        }
        if (session.transaction_failed) {
            session.in_transaction = false;
            session.transaction_failed = false;
            session.queued_commands.clear();
            session.watched_versions.clear();
            return {resp::Value::error(
                "EXECABORT Transaction discarded because of previous errors.")};
        }
        for (const auto& [key, watched_version] : session.watched_versions) {
            if (database_.version(key) != watched_version) {
                session.in_transaction = false;
                session.queued_commands.clear();
                session.watched_versions.clear();
                return {resp::Value::null_array()};
            }
        }
        auto commands = std::move(session.queued_commands);
        session.in_transaction = false;
        session.transaction_failed = false;
        session.watched_versions.clear();
        std::vector<resp::Value> replies;
        std::vector<Delivery> deliveries;
        std::vector<std::vector<std::string>> records;
        replies.reserve(commands.size());
        for (const auto& command : commands) {
            auto result = execute_direct(connection_id, session, command);
            if (persist) {
                const CommandSpec* const queued_spec = find_spec(command.front());
                if (queued_spec != nullptr) {
                    auto canonical = canonical_commands(*queued_spec, command, result, database_);
                    records.insert(records.end(),
                                   std::make_move_iterator(canonical.begin()),
                                   std::make_move_iterator(canonical.end()));
                }
            }
            replies.push_back(std::move(result.reply));
            deliveries.insert(deliveries.end(),
                              std::make_move_iterator(result.deliveries.begin()),
                              std::make_move_iterator(result.deliveries.end()));
        }
        if (persist && persistence_ != nullptr && !records.empty()) {
            std::string error;
            if (!persistence_->append_batch(records, error)) {
                return {resp::Value::error("MISCONF AOF append failed: " + error)};
            }
        }
        return {resp::Value::array(std::move(replies)), false, std::move(deliveries)};
    }

    if (session.in_transaction &&
        (spec->id == CommandId::subscribe || spec->id == CommandId::unsubscribe)) {
        session.transaction_failed = true;
        return {resp::Value::error("ERR pub/sub commands are not allowed inside MULTI")};
    }

    if (session.in_transaction) {
        if (const auto error = validate_syntax(spec->id, arguments); error.has_value()) {
            session.transaction_failed = true;
            return {resp::Value::error(*error)};
        }
        session.queued_commands.push_back(arguments);
        return {resp::Value::simple("QUEUED")};
    }

    if (const auto error = validate_syntax(spec->id, arguments); error.has_value()) {
        return {resp::Value::error(*error)};
    }
    auto result = execute_direct(connection_id, session, arguments);
    if (persist && persistence_ != nullptr) {
        auto records = canonical_commands(*spec, arguments, result, database_);
        if (!records.empty()) {
            std::string error;
            if (!persistence_->append_batch(records, error)) {
                return {resp::Value::error("MISCONF AOF append failed: " + error)};
            }
        }
    }
    return result;
}

void Engine::disconnect(std::uint64_t connection_id, Session& session) {
    const auto iterator = connection_channels_.find(connection_id);
    if (iterator != connection_channels_.end()) {
        for (const auto& channel : iterator->second) {
            auto subscribers = channel_subscribers_.find(channel);
            if (subscribers != channel_subscribers_.end()) {
                subscribers->second.erase(connection_id);
                if (subscribers->second.empty()) {
                    channel_subscribers_.erase(subscribers);
                }
            }
        }
        connection_channels_.erase(iterator);
    }
    session = {};
}

std::size_t Engine::active_expire(std::size_t work_limit) {
    return database_.active_expire(work_limit);
}

storage::Database& Engine::database() noexcept { return database_; }

const storage::Database& Engine::database() const noexcept { return database_; }

std::uint64_t Engine::commands_processed() const noexcept { return commands_processed_; }

Result Engine::execute_direct(std::uint64_t connection_id, Session& session,
                              const std::vector<std::string>& arguments) {
    (void)connection_id;
    (void)session;
    const CommandSpec* const spec = find_spec(arguments.front());
    if (spec == nullptr) {
        return {resp::Value::error("ERR unknown command")};
    }
    const auto id = spec->id;

    if (id == CommandId::ping) {
        const auto channels = connection_channels_.find(connection_id);
        if (channels != connection_channels_.end() && !channels->second.empty()) {
            return {resp::Value::array(
                {resp::Value::bulk("pong"),
                 resp::Value::bulk(arguments.size() == 1U ? "" : arguments[1])})};
        }
        return {arguments.size() == 1U ? resp::Value::simple("PONG")
                                      : resp::Value::bulk(arguments[1])};
    }
    if (id == CommandId::echo) {
        return {resp::Value::bulk(arguments[1])};
    }
    if (id == CommandId::quit) {
        return {resp::Value::simple("OK"), true};
    }
    if (id == CommandId::info) {
        std::string info = "# Server\r\nkeyforge_version:" + std::string{keyforge::version()} +
                           "\r\nredis_compatibility:subset\r\n# Stats\r\ntotal_commands_processed:" +
                           std::to_string(commands_processed_) +
                           "\r\n# Keyspace\r\nkeys:" + std::to_string(database_.size()) +
                           "\r\ndependency_edges:" +
                           std::to_string(database_.dependencies().edge_count()) + "\r\n";
        return {resp::Value::bulk(std::move(info))};
    }
    if (id == CommandId::del) {
        return {resp::Value::number(static_cast<std::int64_t>(database_.erase_many(
            std::vector<std::string>{arguments.begin() + 1, arguments.end()})))};
    }
    if (id == CommandId::exists) {
        std::int64_t count = 0;
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            count += database_.exists(arguments[index]) ? 1 : 0;
        }
        return {resp::Value::number(count)};
    }
    if (id == CommandId::type) {
        const auto* const entry = database_.find(arguments[1]);
        return {resp::Value::simple(entry == nullptr ? "none"
                                                    : std::string{storage::type_name(*entry)})};
    }
    if (id == CommandId::dbsize) {
        return {resp::Value::number(static_cast<std::int64_t>(database_.size()))};
    }
    if (id == CommandId::flushdb) {
        database_.clear();
        return {resp::Value::simple("OK")};
    }
    if (id == CommandId::keys) {
        std::vector<std::string> matches;
        for (auto& key : database_.keys()) {
            if (glob_matches(arguments[1], key)) {
                matches.push_back(std::move(key));
            }
        }
        return {bulk_array(matches)};
    }
    if (id == CommandId::set) {
        enum class Condition { none, nx, xx };
        Condition condition = Condition::none;
        std::optional<std::int64_t> ttl_ms;
        for (std::size_t index = 3; index < arguments.size(); ++index) {
            const std::string option = ascii_upper(arguments[index]);
            if (option == "NX") {
                condition = Condition::nx;
            } else if (option == "XX") {
                condition = Condition::xx;
            } else {
                std::int64_t amount = 0;
                (void)parse_i64(arguments[++index], amount);
                if (option == "EX") {
                    if (!checked_multiply(amount, 1000, amount)) {
                        return {resp::Value::error("ERR invalid expire time in 'set' command")};
                    }
                }
                ttl_ms = amount;
            }
        }
        const bool exists = database_.exists(arguments[1]);
        if ((condition == Condition::nx && exists) || (condition == Condition::xx && !exists)) {
            return {resp::Value::null_bulk()};
        }
        std::int64_t deadline = 0;
        if (ttl_ms.has_value() &&
            !checked_add(database_.clock().unix_milliseconds(), *ttl_ms, deadline)) {
            return {resp::Value::error("ERR invalid expire time in 'set' command")};
        }
        database_.replace(arguments[1], storage::String{arguments[2]});
        if (ttl_ms.has_value()) {
            (void)database_.set_expiration(arguments[1], deadline);
        }
        return {resp::Value::simple("OK")};
    }
    if (id == CommandId::get) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::null_bulk()};
        }
        const auto* const value = std::get_if<storage::String>(&entry->data);
        return value == nullptr ? Result{wrong_type()} : Result{resp::Value::bulk(*value)};
    }
    if (id == CommandId::mget) {
        std::vector<resp::Value> values;
        values.reserve(arguments.size() - 1U);
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            auto* const entry = database_.find(arguments[index]);
            if (entry == nullptr) {
                values.push_back(resp::Value::null_bulk());
                continue;
            }
            const auto* const value = std::get_if<storage::String>(&entry->data);
            if (value == nullptr) {
                return {wrong_type()};
            }
            values.push_back(resp::Value::bulk(*value));
        }
        return {resp::Value::array(std::move(values))};
    }
    if (id == CommandId::mset) {
        for (std::size_t index = 1; index < arguments.size(); index += 2U) {
            database_.replace(arguments[index], storage::String{arguments[index + 1U]});
        }
        return {resp::Value::simple("OK")};
    }
    if (id == CommandId::incr || id == CommandId::decr || id == CommandId::incrby) {
        std::int64_t increment = id == CommandId::decr ? -1 : 1;
        if (id == CommandId::incrby) {
            (void)parse_i64(arguments[2], increment);
        }
        auto* entry = database_.find(arguments[1]);
        std::int64_t current = 0;
        if (entry != nullptr) {
            auto* const value = std::get_if<storage::String>(&entry->data);
            if (value == nullptr) {
                return {wrong_type()};
            }
            if (!parse_i64(*value, current)) {
                return {resp::Value::error("ERR value is not an integer or out of range")};
            }
            std::int64_t updated = 0;
            if (!checked_add(current, increment, updated)) {
                return {resp::Value::error("ERR increment or decrement would overflow")};
            }
            *value = std::to_string(updated);
            database_.touch(arguments[1]);
            return {resp::Value::number(updated)};
        }
        (void)database_.create(arguments[1], storage::String{std::to_string(increment)});
        return {resp::Value::number(increment)};
    }
    if (id == CommandId::append) {
        auto* entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            auto& created = database_.create(arguments[1], storage::String{arguments[2]});
            const auto& value = std::get<storage::String>(created.data);
            return {resp::Value::number(static_cast<std::int64_t>(value.size()))};
        }
        auto* const value = std::get_if<storage::String>(&entry->data);
        if (value == nullptr) {
            return {wrong_type()};
        }
        value->append(arguments[2]);
        database_.touch(arguments[1]);
        return {resp::Value::number(static_cast<std::int64_t>(value->size()))};
    }
    if (id == CommandId::strlen) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::number(0)};
        }
        const auto* const value = std::get_if<storage::String>(&entry->data);
        return value == nullptr ? Result{wrong_type()}
                                : Result{resp::Value::number(
                                      static_cast<std::int64_t>(value->size()))};
    }
    if (id == CommandId::expire || id == CommandId::pexpire || id == CommandId::pexpireat) {
        std::int64_t amount = 0;
        (void)parse_i64(arguments[2], amount);
        if (id == CommandId::expire && !checked_multiply(amount, 1000, amount)) {
            return {resp::Value::error("ERR value is not an integer or out of range")};
        }
        std::int64_t deadline = amount;
        if (id != CommandId::pexpireat &&
            !checked_add(database_.clock().unix_milliseconds(), amount, deadline)) {
            return {resp::Value::error("ERR value is not an integer or out of range")};
        }
        return {resp::Value::number(database_.set_expiration(arguments[1], deadline) ? 1 : 0)};
    }
    if (id == CommandId::ttl || id == CommandId::pttl) {
        if (!database_.exists(arguments[1])) {
            return {resp::Value::number(-2)};
        }
        const auto remaining = database_.remaining_ttl(arguments[1]);
        if (!remaining.has_value()) {
            return {resp::Value::number(-1)};
        }
        return {resp::Value::number(id == CommandId::ttl ? *remaining / 1000 : *remaining)};
    }
    if (id == CommandId::persist) {
        return {resp::Value::number(database_.persist(arguments[1]) ? 1 : 0)};
    }

    if (id == CommandId::lpush || id == CommandId::rpush) {
        auto* entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            entry = &database_.create(arguments[1], storage::List{});
        }
        auto* const list = std::get_if<storage::List>(&entry->data);
        if (list == nullptr) {
            return {wrong_type()};
        }
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            if (id == CommandId::lpush) {
                list->push_front(arguments[index]);
            } else {
                list->push_back(arguments[index]);
            }
        }
        if (entry->data.index() != 0U) {
            database_.touch(arguments[1]);
        }
        return {resp::Value::number(static_cast<std::int64_t>(list->size()))};
    }
    if (id == CommandId::lpop || id == CommandId::rpop) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::null_bulk()};
        }
        auto* const list = std::get_if<storage::List>(&entry->data);
        if (list == nullptr) {
            return {wrong_type()};
        }
        std::string value = id == CommandId::lpop ? std::move(list->front())
                                                  : std::move(list->back());
        if (id == CommandId::lpop) {
            list->pop_front();
        } else {
            list->pop_back();
        }
        if (list->empty()) {
            (void)database_.erase(arguments[1]);
        } else {
            database_.touch(arguments[1]);
        }
        return {resp::Value::bulk(std::move(value))};
    }
    if (id == CommandId::llen) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::number(0)};
        }
        const auto* const list = std::get_if<storage::List>(&entry->data);
        return list == nullptr ? Result{wrong_type()}
                               : Result{resp::Value::number(
                                     static_cast<std::int64_t>(list->size()))};
    }
    if (id == CommandId::lrange) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::array({})};
        }
        const auto* const list = std::get_if<storage::List>(&entry->data);
        if (list == nullptr) {
            return {wrong_type()};
        }
        std::int64_t start = 0;
        std::int64_t stop = 0;
        (void)parse_i64(arguments[2], start);
        (void)parse_i64(arguments[3], stop);
        const auto range = normalize_range(start, stop, list->size());
        std::vector<resp::Value> values;
        if (!range.empty) {
            values.reserve(range.end - range.begin + 1U);
            for (std::size_t index = range.begin; index <= range.end; ++index) {
                values.push_back(resp::Value::bulk((*list)[index]));
            }
        }
        return {resp::Value::array(std::move(values))};
    }

    if (id == CommandId::hset) {
        auto* entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            entry = &database_.create(arguments[1], storage::Hash{});
        }
        auto* const hash = std::get_if<storage::Hash>(&entry->data);
        if (hash == nullptr) {
            return {wrong_type()};
        }
        std::int64_t added = 0;
        bool changed = false;
        for (std::size_t index = 2; index < arguments.size(); index += 2U) {
            const auto existing = hash->find(arguments[index]);
            if (existing == hash->end()) {
                hash->emplace(arguments[index], arguments[index + 1U]);
                ++added;
                changed = true;
            } else if (existing->second != arguments[index + 1U]) {
                existing->second = arguments[index + 1U];
                changed = true;
            }
        }
        if (changed) {
            database_.touch(arguments[1]);
        }
        return {resp::Value::number(added)};
    }
    if (id == CommandId::hget || id == CommandId::hmget) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            if (id == CommandId::hget) {
                return {resp::Value::null_bulk()};
            }
            return {resp::Value::array(std::vector<resp::Value>(arguments.size() - 2U,
                                                               resp::Value::null_bulk()))};
        }
        const auto* const hash = std::get_if<storage::Hash>(&entry->data);
        if (hash == nullptr) {
            return {wrong_type()};
        }
        if (id == CommandId::hget) {
            const auto iterator = hash->find(arguments[2]);
            return {iterator == hash->end() ? resp::Value::null_bulk()
                                            : resp::Value::bulk(iterator->second)};
        }
        std::vector<resp::Value> values;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            const auto iterator = hash->find(arguments[index]);
            values.push_back(iterator == hash->end() ? resp::Value::null_bulk()
                                                      : resp::Value::bulk(iterator->second));
        }
        return {resp::Value::array(std::move(values))};
    }
    if (id == CommandId::hgetall) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::array({})};
        }
        const auto* const hash = std::get_if<storage::Hash>(&entry->data);
        if (hash == nullptr) {
            return {wrong_type()};
        }
        std::vector<std::string> fields;
        fields.reserve(hash->size());
        for (const auto& [field, value] : *hash) {
            (void)value;
            fields.push_back(field);
        }
        std::sort(fields.begin(), fields.end());
        std::vector<resp::Value> values;
        values.reserve(fields.size() * 2U);
        for (const auto& field : fields) {
            values.push_back(resp::Value::bulk(field));
            values.push_back(resp::Value::bulk(hash->at(field)));
        }
        return {resp::Value::array(std::move(values))};
    }
    if (id == CommandId::hdel) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::number(0)};
        }
        auto* const hash = std::get_if<storage::Hash>(&entry->data);
        if (hash == nullptr) {
            return {wrong_type()};
        }
        std::int64_t removed = 0;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            removed += static_cast<std::int64_t>(hash->erase(arguments[index]));
        }
        if (removed > 0) {
            if (hash->empty()) {
                (void)database_.erase(arguments[1]);
            } else {
                database_.touch(arguments[1]);
            }
        }
        return {resp::Value::number(removed)};
    }
    if (id == CommandId::hexists || id == CommandId::hlen) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::number(0)};
        }
        const auto* const hash = std::get_if<storage::Hash>(&entry->data);
        if (hash == nullptr) {
            return {wrong_type()};
        }
        return {resp::Value::number(id == CommandId::hlen
                                        ? static_cast<std::int64_t>(hash->size())
                                        : (hash->contains(arguments[2]) ? 1 : 0))};
    }

    if (id == CommandId::sadd) {
        auto* entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            entry = &database_.create(arguments[1], storage::Set{});
        }
        auto* const set = std::get_if<storage::Set>(&entry->data);
        if (set == nullptr) {
            return {wrong_type()};
        }
        std::int64_t added = 0;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            added += set->insert(arguments[index]).second ? 1 : 0;
        }
        if (added > 0) {
            database_.touch(arguments[1]);
        }
        return {resp::Value::number(added)};
    }
    if (id == CommandId::srem) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::number(0)};
        }
        auto* const set = std::get_if<storage::Set>(&entry->data);
        if (set == nullptr) {
            return {wrong_type()};
        }
        std::int64_t removed = 0;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            removed += static_cast<std::int64_t>(set->erase(arguments[index]));
        }
        if (removed > 0) {
            if (set->empty()) {
                (void)database_.erase(arguments[1]);
            } else {
                database_.touch(arguments[1]);
            }
        }
        return {resp::Value::number(removed)};
    }
    if (id == CommandId::sismember || id == CommandId::scard || id == CommandId::smembers) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {id == CommandId::smembers ? resp::Value::array({})
                                             : resp::Value::number(0)};
        }
        const auto* const set = std::get_if<storage::Set>(&entry->data);
        if (set == nullptr) {
            return {wrong_type()};
        }
        if (id == CommandId::sismember) {
            return {resp::Value::number(set->contains(arguments[2]) ? 1 : 0)};
        }
        if (id == CommandId::scard) {
            return {resp::Value::number(static_cast<std::int64_t>(set->size()))};
        }
        std::vector<std::string> members{set->begin(), set->end()};
        std::sort(members.begin(), members.end());
        return {bulk_array(members)};
    }

    if (id == CommandId::zadd) {
        std::vector<std::pair<double, std::string>> values;
        for (std::size_t index = 2; index < arguments.size(); index += 2U) {
            double score = 0;
            (void)parse_double(arguments[index], score);
            values.emplace_back(score, arguments[index + 1U]);
        }
        auto* entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            entry = &database_.create(arguments[1], storage::SortedSet{});
        }
        auto* const set = std::get_if<storage::SortedSet>(&entry->data);
        if (set == nullptr) {
            return {wrong_type()};
        }
        std::int64_t added = 0;
        bool changed = false;
        for (auto& [score, member] : values) {
            const auto previous = set->score(member);
            added += set->add(member, score) ? 1 : 0;
            if (!previous.has_value() || *previous != score) {
                changed = true;
            }
        }
        if (changed) {
            database_.touch(arguments[1]);
        }
        return {resp::Value::number(added)};
    }
    if (id == CommandId::zrem) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            return {resp::Value::number(0)};
        }
        auto* const set = std::get_if<storage::SortedSet>(&entry->data);
        if (set == nullptr) {
            return {wrong_type()};
        }
        std::int64_t removed = 0;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            removed += set->remove(arguments[index]) ? 1 : 0;
        }
        if (removed > 0) {
            if (set->size() == 0U) {
                (void)database_.erase(arguments[1]);
            } else {
                database_.touch(arguments[1]);
            }
        }
        return {resp::Value::number(removed)};
    }
    if (id == CommandId::zscore || id == CommandId::zcard || id == CommandId::zrange) {
        auto* const entry = database_.find(arguments[1]);
        if (entry == nullptr) {
            if (id == CommandId::zscore) {
                return {resp::Value::null_bulk()};
            }
            return {id == CommandId::zcard ? resp::Value::number(0)
                                          : resp::Value::array({})};
        }
        const auto* const set = std::get_if<storage::SortedSet>(&entry->data);
        if (set == nullptr) {
            return {wrong_type()};
        }
        if (id == CommandId::zscore) {
            const auto score = set->score(arguments[2]);
            return {score.has_value() ? resp::Value::bulk(format_double(*score))
                                      : resp::Value::null_bulk()};
        }
        if (id == CommandId::zcard) {
            return {resp::Value::number(static_cast<std::int64_t>(set->size()))};
        }
        std::int64_t start = 0;
        std::int64_t stop = 0;
        (void)parse_i64(arguments[2], start);
        (void)parse_i64(arguments[3], stop);
        const bool with_scores = arguments.size() == 5U;
        std::vector<resp::Value> output;
        for (const auto& [member, score] : set->range(start, stop)) {
            output.push_back(resp::Value::bulk(member));
            if (with_scores) {
                output.push_back(resp::Value::bulk(format_double(score)));
            }
        }
        return {resp::Value::array(std::move(output))};
    }

    if (id == CommandId::depend) {
        if (!database_.exists(arguments[1])) {
            return {resp::Value::error("ERR dependency child key does not exist")};
        }
        std::vector<std::string> parents;
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            if (!database_.exists(arguments[index])) {
                return {resp::Value::error("ERR dependency parent key does not exist: " +
                                          arguments[index])};
            }
            parents.push_back(arguments[index]);
        }
        std::string error;
        const auto before = database_.dependencies().edge_count();
        if (!database_.dependencies().add_dependencies(arguments[1], parents, error)) {
            return {resp::Value::error("ERR " + error)};
        }
        const auto added = database_.dependencies().edge_count() - before;
        if (added > 0U) {
            database_.touch(arguments[1]);
        }
        return {resp::Value::number(static_cast<std::int64_t>(added))};
    }
    if (id == CommandId::deps || id == CommandId::dependents) {
        const auto values = id == CommandId::deps
                                ? database_.dependencies().parents(arguments[1])
                                : database_.dependencies().dependents(arguments[1]);
        return {bulk_array(values)};
    }
    if (id == CommandId::invalidate) {
        return {resp::Value::number(
            static_cast<std::int64_t>(database_.invalidate(arguments[1])))};
    }
    if (id == CommandId::publish) {
        std::vector<Delivery> deliveries;
        const auto subscribers = channel_subscribers_.find(arguments[1]);
        if (subscribers != channel_subscribers_.end()) {
            std::vector<std::uint64_t> recipients{subscribers->second.begin(),
                                                  subscribers->second.end()};
            std::sort(recipients.begin(), recipients.end());
            deliveries.reserve(recipients.size());
            for (const auto recipient : recipients) {
                deliveries.push_back(
                    {recipient,
                     resp::Value::array({resp::Value::bulk("message"),
                                         resp::Value::bulk(arguments[1]),
                                         resp::Value::bulk(arguments[2])})});
            }
        }
        return {resp::Value::number(static_cast<std::int64_t>(deliveries.size())), false,
                std::move(deliveries)};
    }
    if (id == CommandId::subscribe) {
        std::vector<resp::Value> acknowledgements;
        auto& channels = connection_channels_[connection_id];
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            channels.insert(arguments[index]);
            channel_subscribers_[arguments[index]].insert(connection_id);
            acknowledgements.push_back(resp::Value::array(
                {resp::Value::bulk("subscribe"), resp::Value::bulk(arguments[index]),
                 resp::Value::number(static_cast<std::int64_t>(channels.size()))}));
        }
        std::vector<Delivery> extra;
        for (std::size_t index = 1; index < acknowledgements.size(); ++index) {
            extra.push_back({connection_id, std::move(acknowledgements[index])});
        }
        return {std::move(acknowledgements.front()), false, std::move(extra)};
    }
    if (id == CommandId::unsubscribe) {
        std::vector<std::string> channels;
        auto connection = connection_channels_.find(connection_id);
        if (arguments.size() == 1U) {
            if (connection != connection_channels_.end()) {
                channels.assign(connection->second.begin(), connection->second.end());
                std::sort(channels.begin(), channels.end());
            }
        } else {
            channels.assign(arguments.begin() + 1, arguments.end());
        }
        std::vector<resp::Value> acknowledgements;
        if (channels.empty()) {
            acknowledgements.push_back(resp::Value::array(
                {resp::Value::bulk("unsubscribe"), resp::Value::null_bulk(),
                 resp::Value::number(0)}));
        }
        for (const auto& channel : channels) {
            connection = connection_channels_.find(connection_id);
            if (connection != connection_channels_.end()) {
                connection->second.erase(channel);
                if (connection->second.empty()) {
                    connection_channels_.erase(connection);
                }
            }
            auto subscribers = channel_subscribers_.find(channel);
            if (subscribers != channel_subscribers_.end()) {
                subscribers->second.erase(connection_id);
                if (subscribers->second.empty()) {
                    channel_subscribers_.erase(subscribers);
                }
            }
            const auto remaining = connection_channels_.find(connection_id);
            const auto count = remaining == connection_channels_.end() ? 0U : remaining->second.size();
            acknowledgements.push_back(resp::Value::array(
                {resp::Value::bulk("unsubscribe"), resp::Value::bulk(channel),
                 resp::Value::number(static_cast<std::int64_t>(count))}));
        }
        std::vector<Delivery> extra;
        for (std::size_t index = 1; index < acknowledgements.size(); ++index) {
            extra.push_back({connection_id, std::move(acknowledgements[index])});
        }
        return {std::move(acknowledgements.front()), false, std::move(extra)};
    }
    return {resp::Value::error("ERR command is not executable")};
}

}  // namespace keyforge::commands

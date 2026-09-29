#include "keyforge/commands/engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeClock final : public keyforge::Clock {
public:
    [[nodiscard]] std::int64_t unix_milliseconds() const noexcept override { return unix_now_; }
    [[nodiscard]] std::int64_t monotonic_milliseconds() const noexcept override {
        return monotonic_now_;
    }
    void set(std::int64_t now) noexcept {
        unix_now_ = now;
        monotonic_now_ = now;
    }
    void advance(std::int64_t amount) noexcept {
        unix_now_ += amount;
        monotonic_now_ += amount;
    }
    void jump_wall(std::int64_t amount) noexcept { unix_now_ += amount; }

private:
    std::int64_t unix_now_{1'700'000'000'000LL};
    std::int64_t monotonic_now_{1'700'000'000'000LL};
};

class Harness {
public:
    keyforge::resp::Value run(std::vector<std::string> arguments,
                              keyforge::commands::Session& selected_session) {
        return engine.execute(1, selected_session, arguments).reply;
    }

    keyforge::resp::Value run(std::vector<std::string> arguments) {
        return run(std::move(arguments), session);
    }

    FakeClock clock;
    keyforge::commands::Engine engine{clock};
    keyforge::commands::Session session;
};

void require_error(const keyforge::resp::Value& value, std::string_view prefix) {
    REQUIRE(value.type == keyforge::resp::Type::error);
    CHECK(value.text.starts_with(prefix));
}

}  // namespace

TEST_CASE("string and generic commands preserve Redis-like types and boundaries") {
    Harness harness;
    CHECK(harness.run({"SET", "key", "41"}) == keyforge::resp::Value::simple("OK"));
    CHECK(harness.run({"GET", "key"}) == keyforge::resp::Value::bulk("41"));
    CHECK(harness.run({"INCR", "key"}) == keyforge::resp::Value::number(42));
    CHECK(harness.run({"DECR", "key"}) == keyforge::resp::Value::number(41));
    CHECK(harness.run({"INCRBY", "key", "9"}) == keyforge::resp::Value::number(50));
    CHECK(harness.run({"APPEND", "key", "!"}) == keyforge::resp::Value::number(3));
    CHECK(harness.run({"STRLEN", "key"}) == keyforge::resp::Value::number(3));
    CHECK(harness.run({"TYPE", "key"}) == keyforge::resp::Value::simple("string"));

    CHECK(harness.run({"SET", "key", "no", "NX"}) == keyforge::resp::Value::null_bulk());
    CHECK(harness.run({"SET", "missing", "no", "XX"}) ==
          keyforge::resp::Value::null_bulk());
    require_error(harness.run({"SET", "x", "v", "NX", "XX"}), "ERR syntax");

    CHECK(harness.run({"MSET", "a", "1", "b", "2"}) ==
          keyforge::resp::Value::simple("OK"));
    CHECK(harness.run({"MGET", "a", "absent", "b"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("1"),
                                        keyforge::resp::Value::null_bulk(),
                                        keyforge::resp::Value::bulk("2")}));
    CHECK(harness.run({"EXISTS", "a", "absent", "a"}) ==
          keyforge::resp::Value::number(2));
    CHECK(harness.run({"KEYS", "?"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("a"),
                                        keyforge::resp::Value::bulk("b")}));

    CHECK(harness.run({"SET", "maximum", std::to_string(std::numeric_limits<std::int64_t>::max())}) ==
          keyforge::resp::Value::simple("OK"));
    require_error(harness.run({"INCR", "maximum"}), "ERR increment");
    CHECK(harness.run({"DEL", "a", "b", "a"}) == keyforge::resp::Value::number(2));
}

TEST_CASE("list commands implement inclusive and negative ranges and empty cleanup") {
    Harness harness;
    CHECK(harness.run({"RPUSH", "list", "a", "b", "c"}) ==
          keyforge::resp::Value::number(3));
    CHECK(harness.run({"LPUSH", "list", "x", "y"}) == keyforge::resp::Value::number(5));
    CHECK(harness.run({"LRANGE", "list", "0", "-1"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("y"),
                                        keyforge::resp::Value::bulk("x"),
                                        keyforge::resp::Value::bulk("a"),
                                        keyforge::resp::Value::bulk("b"),
                                        keyforge::resp::Value::bulk("c")}));
    CHECK(harness.run({"LRANGE", "list", "-2", "99"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("b"),
                                        keyforge::resp::Value::bulk("c")}));
    CHECK(harness.run({"LRANGE", "list", "9", "10"}) == keyforge::resp::Value::array({}));
    CHECK(harness.run({"LPOP", "list"}) == keyforge::resp::Value::bulk("y"));
    CHECK(harness.run({"RPOP", "list"}) == keyforge::resp::Value::bulk("c"));

    REQUIRE(harness.run({"DEL", "list"}) == keyforge::resp::Value::number(1));
    REQUIRE(harness.run({"LPUSH", "one", "value"}) == keyforge::resp::Value::number(1));
    CHECK(harness.run({"LPOP", "one"}) == keyforge::resp::Value::bulk("value"));
    CHECK(harness.run({"TYPE", "one"}) == keyforge::resp::Value::simple("none"));
}

TEST_CASE("hash set and sorted-set commands enforce types and clean empty values") {
    Harness harness;
    CHECK(harness.run({"HSET", "hash", "b", "2", "a", "1"}) ==
          keyforge::resp::Value::number(2));
    CHECK(harness.run({"HSET", "hash", "a", "updated"}) ==
          keyforge::resp::Value::number(0));
    CHECK(harness.run({"HGETALL", "hash"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("a"),
                                        keyforge::resp::Value::bulk("updated"),
                                        keyforge::resp::Value::bulk("b"),
                                        keyforge::resp::Value::bulk("2")}));
    CHECK(harness.run({"HMGET", "hash", "b", "missing"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("2"),
                                        keyforge::resp::Value::null_bulk()}));
    CHECK(harness.run({"HDEL", "hash", "a", "b"}) == keyforge::resp::Value::number(2));
    CHECK(harness.run({"TYPE", "hash"}) == keyforge::resp::Value::simple("none"));

    CHECK(harness.run({"SADD", "set", "b", "a", "b"}) ==
          keyforge::resp::Value::number(2));
    CHECK(harness.run({"SMEMBERS", "set"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("a"),
                                        keyforge::resp::Value::bulk("b")}));
    CHECK(harness.run({"SISMEMBER", "set", "b"}) == keyforge::resp::Value::number(1));
    CHECK(harness.run({"SREM", "set", "a", "b"}) == keyforge::resp::Value::number(2));
    CHECK(harness.run({"SCARD", "set"}) == keyforge::resp::Value::number(0));

    CHECK(harness.run({"ZADD", "z", "2", "b", "1", "z", "1", "a"}) ==
          keyforge::resp::Value::number(3));
    CHECK(harness.run({"ZRANGE", "z", "0", "-1", "WITHSCORES"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("a"),
                                        keyforge::resp::Value::bulk("1"),
                                        keyforge::resp::Value::bulk("z"),
                                        keyforge::resp::Value::bulk("1"),
                                        keyforge::resp::Value::bulk("b"),
                                        keyforge::resp::Value::bulk("2")}));
    CHECK(harness.run({"ZADD", "z", "0", "b"}) == keyforge::resp::Value::number(0));
    CHECK(harness.run({"ZRANGE", "z", "0", "0"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("b")}));
    require_error(harness.run({"ZADD", "z", "nan", "bad"}), "ERR value");
    require_error(harness.run({"GET", "z"}), "WRONGTYPE");
}

TEST_CASE("expiration is deterministic at the exact boundary and ignores stale heap records") {
    Harness harness;
    REQUIRE(harness.run({"SET", "key", "value"}) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"PEXPIRE", "key", "10"}) == keyforge::resp::Value::number(1));
    REQUIRE(harness.run({"PEXPIRE", "key", "100"}) == keyforge::resp::Value::number(1));
    harness.clock.advance(10);
    CHECK(harness.engine.active_expire(10) == 0);
    CHECK(harness.run({"GET", "key"}) == keyforge::resp::Value::bulk("value"));
    CHECK(harness.run({"PTTL", "key"}) == keyforge::resp::Value::number(90));
    harness.clock.advance(90);
    CHECK(harness.run({"GET", "key"}) == keyforge::resp::Value::null_bulk());
    CHECK(harness.run({"TTL", "key"}) == keyforge::resp::Value::number(-2));

    REQUIRE(harness.run({"SET", "persistent", "v", "PX", "500"}) ==
            keyforge::resp::Value::simple("OK"));
    CHECK(harness.run({"PERSIST", "persistent"}) == keyforge::resp::Value::number(1));
    harness.clock.advance(500);
    CHECK(harness.run({"GET", "persistent"}) == keyforge::resp::Value::bulk("v"));
}

TEST_CASE("runtime TTL uses monotonic time across a wall-clock jump") {
    Harness harness;
    REQUIRE(harness.run({"SET", "key", "value", "PX", "100"}) ==
            keyforge::resp::Value::simple("OK"));
    harness.clock.jump_wall(60'000);
    CHECK(harness.run({"PTTL", "key"}) == keyforge::resp::Value::number(100));
    harness.clock.advance(100);
    CHECK(harness.run({"GET", "key"}) == keyforge::resp::Value::null_bulk());
}

TEST_CASE("WATCH detects absent-key mutation and expiry while successful EXEC is atomic") {
    Harness harness;
    keyforge::commands::Session first;
    keyforge::commands::Session second;

    REQUIRE(harness.run({"WATCH", "absent"}, first) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"MULTI"}, first) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"SET", "result", "queued"}, first) ==
            keyforge::resp::Value::simple("QUEUED"));
    REQUIRE(harness.run({"SET", "absent", "changed"}, second) ==
            keyforge::resp::Value::simple("OK"));
    CHECK(harness.run({"EXEC"}, first) == keyforge::resp::Value::null_array());
    CHECK(harness.run({"GET", "result"}) == keyforge::resp::Value::null_bulk());

    REQUIRE(harness.run({"SET", "watched", "v", "PX", "5"}) ==
            keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"WATCH", "watched"}, first) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"MULTI"}, first) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"PING"}, first) == keyforge::resp::Value::simple("QUEUED"));
    harness.clock.advance(5);
    CHECK(harness.run({"EXEC"}, first) == keyforge::resp::Value::null_array());

    REQUIRE(harness.run({"MULTI"}, first) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"SET", "x", "1"}, first) ==
            keyforge::resp::Value::simple("QUEUED"));
    REQUIRE(harness.run({"INCR", "x"}, first) == keyforge::resp::Value::simple("QUEUED"));
    CHECK(harness.run({"EXEC"}, first) ==
          keyforge::resp::Value::array({keyforge::resp::Value::simple("OK"),
                                        keyforge::resp::Value::number(2)}));

    REQUIRE(harness.run({"MULTI"}, first) == keyforge::resp::Value::simple("OK"));
    require_error(harness.run({"SET", "bad", "v", "NX", "XX"}, first), "ERR syntax");
    require_error(harness.run({"EXEC"}, first), "EXECABORT");
}

TEST_CASE("dependency graph rejects cycles and invalidates diamonds and expiry cascades") {
    Harness harness;
    for (const auto& key : {"root", "left", "right", "leaf"}) {
        REQUIRE(harness.run({"SET", key, "cached"}) == keyforge::resp::Value::simple("OK"));
    }
    CHECK(harness.run({"KFG.DEPEND", "left", "root"}) == keyforge::resp::Value::number(1));
    CHECK(harness.run({"KFG.DEPEND", "right", "root"}) == keyforge::resp::Value::number(1));
    CHECK(harness.run({"KFG.DEPEND", "leaf", "left", "right"}) ==
          keyforge::resp::Value::number(2));
    CHECK(harness.run({"KFG.DEPS", "leaf"}) ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("left"),
                                        keyforge::resp::Value::bulk("right")}));
    require_error(harness.run({"KFG.DEPEND", "root", "leaf"}), "ERR dependency");
    CHECK(harness.run({"KFG.INVALIDATE", "root"}) == keyforge::resp::Value::number(4));
    CHECK(harness.run({"DBSIZE"}) == keyforge::resp::Value::number(0));

    REQUIRE(harness.run({"SET", "parent", "v"}) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"SET", "child", "v"}) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"KFG.DEPEND", "child", "parent"}) ==
            keyforge::resp::Value::number(1));
    REQUIRE(harness.run({"PEXPIRE", "parent", "1"}) == keyforge::resp::Value::number(1));
    harness.clock.advance(1);
    CHECK(harness.engine.active_expire(10) == 2);
    CHECK(harness.run({"EXISTS", "parent", "child"}) == keyforge::resp::Value::number(0));
}

TEST_CASE("dependency graph handles a deep iterative cascade") {
    Harness harness;
    constexpr int nodes = 2000;
    for (int index = 0; index < nodes; ++index) {
        REQUIRE(harness.run({"SET", "n" + std::to_string(index), "v"}).type ==
                keyforge::resp::Type::simple_string);
        if (index > 0) {
            REQUIRE(harness.run({"KFG.DEPEND", "n" + std::to_string(index),
                                 "n" + std::to_string(index - 1)}) ==
                    keyforge::resp::Value::number(1));
        }
    }
    CHECK(harness.run({"KFG.INVALIDATE", "n0"}) == keyforge::resp::Value::number(nodes));
}

TEST_CASE("dependency graph handles duplicates deletion transactions and WATCH cascades") {
    Harness harness;
    keyforge::commands::Session transaction;
    keyforge::commands::Session observer;

    REQUIRE(harness.run({"SET", "parent", "v"}) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"SET", "child", "v"}) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"KFG.DEPEND", "child", "parent"}) ==
            keyforge::resp::Value::number(1));
    CHECK(harness.run({"KFG.DEPEND", "child", "parent", "parent"}) ==
          keyforge::resp::Value::number(0));
    REQUIRE(harness.run({"DEL", "parent"}) == keyforge::resp::Value::number(1));
    CHECK(harness.run({"KFG.DEPS", "child"}) == keyforge::resp::Value::array({}));

    REQUIRE(harness.run({"SET", "parent", "new"}) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"MULTI"}, transaction) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"KFG.DEPEND", "child", "parent"}, transaction) ==
            keyforge::resp::Value::simple("QUEUED"));
    CHECK(harness.run({"EXEC"}, transaction) ==
          keyforge::resp::Value::array({keyforge::resp::Value::number(1)}));

    REQUIRE(harness.run({"WATCH", "child"}, observer) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"MULTI"}, observer) == keyforge::resp::Value::simple("OK"));
    REQUIRE(harness.run({"PING"}, observer) == keyforge::resp::Value::simple("QUEUED"));
    REQUIRE(harness.run({"KFG.INVALIDATE", "parent"}) == keyforge::resp::Value::number(2));
    CHECK(harness.run({"EXEC"}, observer) == keyforge::resp::Value::null_array());
}

TEST_CASE("pubsub maintains both indexes and produces asynchronous deliveries") {
    Harness harness;
    keyforge::commands::Session subscriber;
    keyforge::commands::Session publisher;

    auto subscribed = harness.engine.execute(10, subscriber, {"SUBSCRIBE", "news", "alerts"});
    CHECK(subscribed.reply ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("subscribe"),
                                        keyforge::resp::Value::bulk("news"),
                                        keyforge::resp::Value::number(1)}));
    REQUIRE(subscribed.deliveries.size() == 1);
    CHECK(subscribed.deliveries[0].connection_id == 10);

    require_error(harness.engine.execute(10, subscriber, {"GET", "key"}).reply,
                  "ERR only");
    auto published = harness.engine.execute(20, publisher, {"PUBLISH", "news", "hello"});
    CHECK(published.reply == keyforge::resp::Value::number(1));
    REQUIRE(published.deliveries.size() == 1);
    CHECK(published.deliveries[0].connection_id == 10);
    CHECK(published.deliveries[0].message ==
          keyforge::resp::Value::array({keyforge::resp::Value::bulk("message"),
                                        keyforge::resp::Value::bulk("news"),
                                        keyforge::resp::Value::bulk("hello")}));

    harness.engine.disconnect(10, subscriber);
    CHECK(harness.engine.execute(20, publisher, {"PUBLISH", "news", "after"}).reply ==
          keyforge::resp::Value::number(0));
}

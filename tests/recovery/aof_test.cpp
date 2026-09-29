#include "keyforge/commands/engine.hpp"
#include "keyforge/persistence/aof.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class FakeClock final : public keyforge::Clock {
public:
    explicit FakeClock(std::int64_t now) : now_(now) {}
    [[nodiscard]] std::int64_t unix_milliseconds() const noexcept override { return now_; }
    [[nodiscard]] std::int64_t monotonic_milliseconds() const noexcept override { return now_; }
    void set(std::int64_t now) noexcept { now_ = now; }

private:
    std::int64_t now_;
};

class MemorySink final : public keyforge::persistence::Sink {
public:
    bool append_batch(const std::vector<std::vector<std::string>>& commands,
                      std::string& error) override {
        (void)error;
        batches.push_back(commands);
        return true;
    }

    bool flush(std::string& error) override {
        (void)error;
        return true;
    }

    std::vector<std::vector<std::vector<std::string>>> batches;
};

class TempDirectory {
public:
    TempDirectory() {
        char pattern[] = "/tmp/keyforge-aof-test-XXXXXX";
        const char* const created = ::mkdtemp(pattern);
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed: " + std::string{std::strerror(errno)});
        }
        path_ = created;
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool contains_error(const keyforge::resp::Value& value, std::string& error) {
    if (value.type == keyforge::resp::Type::error) {
        error = value.text;
        return true;
    }
    if (value.type == keyforge::resp::Type::array) {
        for (const auto& element : value.elements) {
            if (contains_error(element, error)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool replay_into(keyforge::persistence::Aof& aof,
                               keyforge::commands::Engine& engine, std::string& error) {
    keyforge::commands::Session session;
    engine.begin_replay();
    const bool replayed = aof.replay(
        [&engine, &session](const std::vector<std::string>& command, std::string& command_error) {
            const auto result = engine.execute_replay(session, command);
            return !contains_error(result.reply, command_error);
        },
        error);
    engine.finish_replay();
    return replayed;
}

[[nodiscard]] keyforge::resp::Value run(keyforge::commands::Engine& engine,
                                        keyforge::commands::Session& session,
                                        std::vector<std::string> command) {
    return engine.execute(1, session, command).reply;
}

}  // namespace

TEST_CASE("AOF canonicalizes relative TTL as an absolute deadline") {
    FakeClock clock{1000};
    keyforge::commands::Engine engine{clock};
    keyforge::commands::Session session;
    MemorySink sink;
    engine.set_persistence(&sink);

    REQUIRE(run(engine, session, {"SET", "key", "value", "PX", "250"}) ==
            keyforge::resp::Value::simple("OK"));
    REQUIRE(sink.batches.size() == 1);
    CHECK(sink.batches[0] ==
          std::vector<std::vector<std::string>>{{"SET", "key", "value"},
                                                {"PEXPIREAT", "key", "1250"}});
}

TEST_CASE("AOF restart restores values TTL and dependency relationships") {
    TempDirectory directory;
    const auto path = directory.path() / "appendonly.aof";
    keyforge::resp::Limits limits;
    FakeClock clock{10'000};

    {
        keyforge::persistence::Aof aof{path, keyforge::persistence::FsyncPolicy::always, limits};
        std::string error;
        REQUIRE(aof.open(error));
        keyforge::commands::Engine engine{clock};
        engine.set_persistence(&aof);
        keyforge::commands::Session session;
        REQUIRE(run(engine, session, {"SET", "parent", "source", "PX", "100"}) ==
                keyforge::resp::Value::simple("OK"));
        REQUIRE(run(engine, session, {"SET", "child", "derived"}) ==
                keyforge::resp::Value::simple("OK"));
        REQUIRE(run(engine, session, {"KFG.DEPEND", "child", "parent"}) ==
                keyforge::resp::Value::number(1));
        REQUIRE(aof.flush(error));
    }

    clock.set(10'050);
    {
        keyforge::persistence::Aof aof{path, keyforge::persistence::FsyncPolicy::no, limits};
        std::string error;
        REQUIRE(aof.open(error));
        keyforge::commands::Engine recovered{clock};
        REQUIRE(replay_into(aof, recovered, error));
        keyforge::commands::Session session;
        CHECK(run(recovered, session, {"GET", "parent"}) ==
              keyforge::resp::Value::bulk("source"));
        CHECK(run(recovered, session, {"PTTL", "parent"}) ==
              keyforge::resp::Value::number(50));
        CHECK(run(recovered, session, {"KFG.DEPS", "child"}) ==
              keyforge::resp::Value::array({keyforge::resp::Value::bulk("parent")}));
    }

    clock.set(10'100);
    {
        keyforge::persistence::Aof aof{path, keyforge::persistence::FsyncPolicy::no, limits};
        std::string error;
        REQUIRE(aof.open(error));
        keyforge::commands::Engine recovered{clock};
        REQUIRE(replay_into(aof, recovered, error));
        keyforge::commands::Session session;
        CHECK(run(recovered, session, {"DBSIZE"}) == keyforge::resp::Value::number(0));
    }
}

TEST_CASE("AOF trims an interrupted final record but rejects earlier corruption") {
    TempDirectory directory;
    const auto path = directory.path() / "appendonly.aof";
    keyforge::resp::Limits limits;
    FakeClock clock{1000};

    {
        keyforge::persistence::Aof aof{path, keyforge::persistence::FsyncPolicy::always, limits};
        std::string error;
        REQUIRE(aof.open(error));
        REQUIRE(aof.append_batch({{"SET", "safe", "value"}}, error));
    }
    const auto safe_size = std::filesystem::file_size(path);
    {
        std::ofstream stream{path, std::ios::binary | std::ios::app};
        REQUIRE(stream.good());
        stream << "*1\r\n$5\r\nMUL";
    }
    REQUIRE(std::filesystem::file_size(path) > safe_size);
    {
        keyforge::persistence::Aof aof{path, keyforge::persistence::FsyncPolicy::no, limits};
        std::string error;
        REQUIRE(aof.open(error));
        keyforge::commands::Engine recovered{clock};
        REQUIRE(replay_into(aof, recovered, error));
        CHECK(std::filesystem::file_size(path) == safe_size);
        keyforge::commands::Session session;
        CHECK(run(recovered, session, {"GET", "safe"}) ==
              keyforge::resp::Value::bulk("value"));
    }

    {
        std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
        REQUIRE(stream.good());
        stream.seekp(0);
        stream.put('!');
    }
    {
        keyforge::persistence::Aof aof{path, keyforge::persistence::FsyncPolicy::no, limits};
        std::string error;
        REQUIRE(aof.open(error));
        keyforge::commands::Engine recovered{clock};
        CHECK_FALSE(replay_into(aof, recovered, error));
        CHECK(error.find("AOF corruption") != std::string::npos);
    }
}

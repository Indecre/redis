#include "keyforge/protocol/resp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using keyforge::resp::ParseStatus;
using keyforge::resp::Parser;
using keyforge::resp::Value;

TEST_CASE("RESP encoder supports every required value type") {
    CHECK(keyforge::resp::encode(Value::simple("OK")) == "+OK\r\n");
    CHECK(keyforge::resp::encode(Value::error("ERR broken")) == "-ERR broken\r\n");
    CHECK(keyforge::resp::encode(Value::number(-42)) == ":-42\r\n");
    CHECK(keyforge::resp::encode(Value::bulk(std::string{"a\0b", 3})) ==
          std::string{"$3\r\na\0b\r\n", 9});
    CHECK(keyforge::resp::encode(Value::null_bulk()) == "$-1\r\n");
    CHECK(keyforge::resp::encode(Value::array({Value::bulk("x"), Value::number(7)})) ==
          "*2\r\n$1\r\nx\r\n:7\r\n");
    CHECK(keyforge::resp::encode(Value::null_array()) == "*-1\r\n");
}

TEST_CASE("RESP parser is incremental at every byte boundary") {
    const std::vector<Value> values{
        Value::simple("PONG"),
        Value::error("ERR nope"),
        Value::number(std::numeric_limits<std::int64_t>::min()),
        Value::bulk(std::string{"binary\0value", 12}),
        Value::null_bulk(),
        Value::array({Value::bulk("SET"), Value::bulk("key"), Value::bulk("value")}),
        Value::array({Value::array({Value::number(1)}), Value::null_array()}),
    };

    for (const auto& expected : values) {
        const std::string wire = keyforge::resp::encode(expected);
        for (std::size_t split = 0; split < wire.size(); ++split) {
            Parser parser;
            REQUIRE(parser.feed(std::string_view{wire}.substr(0, split)));
            CHECK(parser.next().status == ParseStatus::incomplete);
            REQUIRE(parser.feed(std::string_view{wire}.substr(split)));
            auto result = parser.next();
            REQUIRE(result.status == ParseStatus::complete);
            REQUIRE(result.value.has_value());
            CHECK(*result.value == expected);
            CHECK(parser.buffered_bytes() == 0);
        }
    }
}

TEST_CASE("RESP parser retains pipelined frames") {
    Parser parser;
    REQUIRE(parser.feed("+OK\r\n:9\r\n$3\r\nhey\r\n"));
    CHECK(parser.next().value == Value::simple("OK"));
    CHECK(parser.next().value == Value::number(9));
    CHECK(parser.next().value == Value::bulk("hey"));
    CHECK(parser.next().status == ParseStatus::incomplete);
}

TEST_CASE("RESP parser rejects malformed input and numeric overflow") {
    const std::vector<std::string> malformed{
        "!wat\r\n",       ":+1\r\n",       ":1x\r\n",
        ":9223372036854775808\r\n", "$-2\r\n",       "*-9\r\n",
        "$3\r\nabcXX",    "+bad\n",        "$x\r\n",
    };
    for (const auto& wire : malformed) {
        Parser parser;
        REQUIRE(parser.feed(wire));
        INFO(wire);
        CHECK(parser.next().status == ParseStatus::invalid);
    }
}

TEST_CASE("RESP parser enforces configured limits") {
    keyforge::resp::Limits limits;
    limits.max_bulk_bytes = 3;
    limits.max_array_length = 2;
    limits.max_arguments = 2;
    limits.max_buffer_bytes = 16;
    limits.max_line_bytes = 8;

    Parser bulk_parser{limits};
    REQUIRE(bulk_parser.feed("$4\r\ntest\r\n"));
    CHECK(bulk_parser.next().status == ParseStatus::invalid);

    Parser array_parser{limits};
    REQUIRE(array_parser.feed("*3\r\n:1\r\n:2\r\n:3\r\n"));
    CHECK(array_parser.next().status == ParseStatus::invalid);

    Parser buffer_parser{limits};
    CHECK_FALSE(buffer_parser.feed(std::string(17, 'x')));
    CHECK(buffer_parser.next().status == ParseStatus::invalid);
}

TEST_CASE("command encoding preserves argv boundaries and binary bytes") {
    const auto wire = keyforge::resp::encode_command({"SET", "a b", std::string{"x\0y", 3}});
    Parser parser;
    REQUIRE(parser.feed(wire));
    const auto result = parser.next();
    REQUIRE(result.value.has_value());
    CHECK(*result.value == Value::array(
                               {Value::bulk("SET"), Value::bulk("a b"),
                                Value::bulk(std::string{"x\0y", 3})}));
}


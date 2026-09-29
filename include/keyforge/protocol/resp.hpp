#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace keyforge::resp {

enum class Type {
    simple_string,
    error,
    integer,
    bulk_string,
    null_bulk_string,
    array,
    null_array,
};

struct Value {
    Type type{Type::simple_string};
    std::string text;
    std::int64_t integer{0};
    std::vector<Value> elements;

    [[nodiscard]] static Value simple(std::string value);
    [[nodiscard]] static Value error(std::string value);
    [[nodiscard]] static Value number(std::int64_t value);
    [[nodiscard]] static Value bulk(std::string value);
    [[nodiscard]] static Value null_bulk();
    [[nodiscard]] static Value array(std::vector<Value> value);
    [[nodiscard]] static Value null_array();

    friend bool operator==(const Value&, const Value&) = default;
};

struct Limits {
    std::size_t max_bulk_bytes{64U * 1024U * 1024U};
    std::size_t max_array_length{1024U * 1024U};
    std::size_t max_arguments{1024U * 1024U};
    std::size_t max_buffer_bytes{128U * 1024U * 1024U};
    std::size_t max_nesting_depth{128U};
    std::size_t max_line_bytes{64U * 1024U};
};

enum class ParseStatus { complete, incomplete, invalid };

struct ParseResult {
    ParseStatus status{ParseStatus::incomplete};
    std::optional<Value> value;
    std::string error;
};

class Parser {
public:
    explicit Parser(Limits limits = {});

    [[nodiscard]] bool feed(std::string_view bytes);
    [[nodiscard]] ParseResult next();
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] std::uint64_t consumed_bytes() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;
    void reset();

private:
    Limits limits_;
    std::string buffer_;
    std::string error_;
    std::uint64_t consumed_bytes_{0};
};

[[nodiscard]] std::string encode(const Value& value);
[[nodiscard]] std::string encode_command(const std::vector<std::string>& arguments);

}  // namespace keyforge::resp

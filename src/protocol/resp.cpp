#include "keyforge/protocol/resp.hpp"

#include <charconv>
#include <limits>
#include <system_error>
#include <utility>

namespace keyforge::resp {
namespace {

struct InternalResult {
    ParseStatus status{ParseStatus::incomplete};
    Value value;
    std::size_t next_offset{0};
    std::string error;
};

[[nodiscard]] InternalResult invalid(std::string message) {
    InternalResult result;
    result.status = ParseStatus::invalid;
    result.error = std::move(message);
    return result;
}

[[nodiscard]] bool has_bad_line_ending(std::string_view input, std::size_t start) {
    const auto newline = input.find('\n', start);
    if (newline != std::string_view::npos &&
        (newline == start || input[newline - 1U] != '\r')) {
        return true;
    }
    const auto carriage = input.find('\r', start);
    return carriage != std::string_view::npos && carriage + 1U < input.size() &&
           input[carriage + 1U] != '\n';
}

struct LineResult {
    ParseStatus status{ParseStatus::incomplete};
    std::string_view line;
    std::size_t next_offset{0};
    std::string error;
};

[[nodiscard]] LineResult read_line(std::string_view input, std::size_t start,
                                   const Limits& limits) {
    const auto end = input.find("\r\n", start);
    if (end == std::string_view::npos) {
        if (has_bad_line_ending(input, start)) {
            return {ParseStatus::invalid, {}, 0, "invalid RESP line ending"};
        }
        if (input.size() - start > limits.max_line_bytes) {
            return {ParseStatus::invalid, {}, 0, "RESP line exceeds configured limit"};
        }
        return {};
    }
    if (end - start > limits.max_line_bytes) {
        return {ParseStatus::invalid, {}, 0, "RESP line exceeds configured limit"};
    }
    return {ParseStatus::complete, input.substr(start, end - start), end + 2U, {}};
}

[[nodiscard]] bool parse_integer(std::string_view input, std::int64_t& output) {
    if (input.empty() || input.front() == '+') {
        return false;
    }
    const char* const begin = input.data();
    const char* const end = begin + input.size();
    const auto result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] InternalResult parse_at(std::string_view input, std::size_t offset,
                                      const Limits& limits, std::size_t depth) {
    if (offset >= input.size()) {
        return {};
    }
    if (depth > limits.max_nesting_depth) {
        return invalid("RESP nesting depth exceeds configured limit");
    }

    const char marker = input[offset];
    if (marker == '+' || marker == '-' || marker == ':') {
        auto line = read_line(input, offset + 1U, limits);
        if (line.status == ParseStatus::invalid) {
            return invalid(std::move(line.error));
        }
        if (line.status == ParseStatus::incomplete) {
            return {};
        }
        InternalResult result;
        result.status = ParseStatus::complete;
        result.next_offset = line.next_offset;
        if (marker == '+') {
            result.value = Value::simple(std::string{line.line});
        } else if (marker == '-') {
            result.value = Value::error(std::string{line.line});
        } else {
            std::int64_t number_value = 0;
            if (!parse_integer(line.line, number_value)) {
                return invalid("invalid RESP integer");
            }
            result.value = Value::number(number_value);
        }
        return result;
    }

    if (marker != '$' && marker != '*') {
        return invalid("unknown RESP type byte");
    }

    auto line = read_line(input, offset + 1U, limits);
    if (line.status == ParseStatus::invalid) {
        return invalid(std::move(line.error));
    }
    if (line.status == ParseStatus::incomplete) {
        return {};
    }
    std::int64_t signed_length = 0;
    if (!parse_integer(line.line, signed_length)) {
        return invalid("invalid RESP length");
    }
    if (signed_length == -1) {
        InternalResult result;
        result.status = ParseStatus::complete;
        result.value = marker == '$' ? Value::null_bulk() : Value::null_array();
        result.next_offset = line.next_offset;
        return result;
    }
    if (signed_length < 0) {
        return invalid("RESP length must be non-negative or -1");
    }
    const auto length = static_cast<std::uint64_t>(signed_length);

    if (marker == '$') {
        if (length > limits.max_bulk_bytes) {
            return invalid("bulk string exceeds configured limit");
        }
        const auto payload_length = static_cast<std::size_t>(length);
        const auto remaining = input.size() - line.next_offset;
        if (remaining < payload_length || remaining - payload_length < 2U) {
            return {};
        }
        const auto terminator = line.next_offset + payload_length;
        if (input[terminator] != '\r' || input[terminator + 1U] != '\n') {
            return invalid("bulk string has invalid terminator");
        }
        InternalResult result;
        result.status = ParseStatus::complete;
        result.value = Value::bulk(std::string{input.substr(line.next_offset, payload_length)});
        result.next_offset = terminator + 2U;
        return result;
    }

    if (length > limits.max_array_length || length > limits.max_arguments) {
        return invalid("array length exceeds configured limit");
    }
    std::vector<Value> elements;
    elements.reserve(static_cast<std::size_t>(length));
    std::size_t current = line.next_offset;
    for (std::uint64_t index = 0; index < length; ++index) {
        auto child = parse_at(input, current, limits, depth + 1U);
        if (child.status != ParseStatus::complete) {
            return child;
        }
        current = child.next_offset;
        elements.push_back(std::move(child.value));
    }
    InternalResult result;
    result.status = ParseStatus::complete;
    result.value = Value::array(std::move(elements));
    result.next_offset = current;
    return result;
}

void append_encoded(const Value& value, std::string& output) {
    switch (value.type) {
        case Type::simple_string:
            output.push_back('+');
            output.append(value.text);
            output.append("\r\n");
            return;
        case Type::error:
            output.push_back('-');
            output.append(value.text);
            output.append("\r\n");
            return;
        case Type::integer: {
            output.push_back(':');
            char buffer[32]{};
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.integer);
            output.append(buffer, result.ptr);
            output.append("\r\n");
            return;
        }
        case Type::bulk_string:
            output.push_back('$');
            output.append(std::to_string(value.text.size()));
            output.append("\r\n");
            output.append(value.text);
            output.append("\r\n");
            return;
        case Type::null_bulk_string:
            output.append("$-1\r\n");
            return;
        case Type::array:
            output.push_back('*');
            output.append(std::to_string(value.elements.size()));
            output.append("\r\n");
            for (const auto& element : value.elements) {
                append_encoded(element, output);
            }
            return;
        case Type::null_array:
            output.append("*-1\r\n");
            return;
    }
}

}  // namespace

Value Value::simple(std::string value) {
    Value result;
    result.type = Type::simple_string;
    result.text = std::move(value);
    return result;
}

Value Value::error(std::string value) {
    Value result;
    result.type = Type::error;
    result.text = std::move(value);
    return result;
}

Value Value::number(std::int64_t value) {
    Value result;
    result.type = Type::integer;
    result.integer = value;
    return result;
}

Value Value::bulk(std::string value) {
    Value result;
    result.type = Type::bulk_string;
    result.text = std::move(value);
    return result;
}

Value Value::null_bulk() {
    Value result;
    result.type = Type::null_bulk_string;
    return result;
}

Value Value::array(std::vector<Value> value) {
    Value result;
    result.type = Type::array;
    result.elements = std::move(value);
    return result;
}

Value Value::null_array() {
    Value result;
    result.type = Type::null_array;
    return result;
}

Parser::Parser(Limits limits) : limits_(limits) {}

bool Parser::feed(std::string_view bytes) {
    if (!error_.empty()) {
        return false;
    }
    if (bytes.size() > limits_.max_buffer_bytes ||
        buffer_.size() > limits_.max_buffer_bytes - bytes.size()) {
        error_ = "client input buffer exceeds configured limit";
        return false;
    }
    buffer_.append(bytes);
    return true;
}

ParseResult Parser::next() {
    if (!error_.empty()) {
        return {ParseStatus::invalid, std::nullopt, error_};
    }
    auto parsed = parse_at(buffer_, 0, limits_, 0);
    if (parsed.status == ParseStatus::invalid) {
        error_ = std::move(parsed.error);
        return {ParseStatus::invalid, std::nullopt, error_};
    }
    if (parsed.status == ParseStatus::incomplete) {
        return {};
    }
    Value value = std::move(parsed.value);
    consumed_bytes_ += parsed.next_offset;
    buffer_.erase(0, parsed.next_offset);
    return {ParseStatus::complete, std::move(value), {}};
}

std::size_t Parser::buffered_bytes() const noexcept { return buffer_.size(); }

std::uint64_t Parser::consumed_bytes() const noexcept { return consumed_bytes_; }

std::string_view Parser::error() const noexcept { return error_; }

void Parser::reset() {
    buffer_.clear();
    error_.clear();
    consumed_bytes_ = 0;
}

std::string encode(const Value& value) {
    std::string output;
    append_encoded(value, output);
    return output;
}

std::string encode_command(const std::vector<std::string>& arguments) {
    std::vector<Value> elements;
    elements.reserve(arguments.size());
    for (const auto& argument : arguments) {
        elements.push_back(Value::bulk(argument));
    }
    return encode(Value::array(std::move(elements)));
}

}  // namespace keyforge::resp

#include "keyforge/protocol/resp.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    keyforge::resp::Limits limits;
    limits.max_bulk_bytes = 64U * 1024U;
    limits.max_array_length = 4096U;
    limits.max_arguments = 4096U;
    limits.max_buffer_bytes = 128U * 1024U;
    limits.max_nesting_depth = 64U;
    keyforge::resp::Parser parser{limits};

    const std::string_view input{reinterpret_cast<const char*>(data), size};
    std::size_t offset = 0;
    std::size_t stride = size == 0 ? 1U : static_cast<std::size_t>(data[0]) + 1U;
    while (offset < input.size()) {
        const auto chunk = std::min(stride, input.size() - offset);
        if (!parser.feed(input.substr(offset, chunk))) {
            break;
        }
        offset += chunk;
        for (;;) {
            const auto result = parser.next();
            if (result.status != keyforge::resp::ParseStatus::complete) {
                break;
            }
        }
        stride = stride % 257U + 1U;
    }
    (void)parser.next();
    return 0;
}

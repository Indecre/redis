#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace keyforge::storage {

class SortedSet {
public:
    [[nodiscard]] bool add(std::string member, double score);
    [[nodiscard]] bool remove(std::string_view member);
    [[nodiscard]] std::optional<double> score(std::string_view member) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string, double>> range(long long start,
                                                                    long long stop) const;

private:
    struct ScoreMemberLess {
        [[nodiscard]] bool operator()(const std::pair<double, std::string>& left,
                                      const std::pair<double, std::string>& right) const noexcept;
    };

    std::unordered_map<std::string, double> scores_;
    std::set<std::pair<double, std::string>, ScoreMemberLess> ordered_;
};

}  // namespace keyforge::storage


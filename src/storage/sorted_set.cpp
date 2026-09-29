#include "keyforge/storage/sorted_set.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace keyforge::storage {
namespace {

struct Range {
    std::size_t begin{0};
    std::size_t end{0};
    bool empty{true};
};

[[nodiscard]] Range normalize_range(long long start, long long stop, std::size_t size) {
    if (size == 0U) {
        return {};
    }
    const auto safe_size = static_cast<long long>(
        std::min(size, static_cast<std::size_t>(std::numeric_limits<long long>::max())));
    if (start < 0) {
        start = std::max(0LL, safe_size + start);
    }
    if (stop < 0) {
        stop = safe_size + stop;
    }
    if (start >= safe_size || stop < 0 || start > stop) {
        return {};
    }
    stop = std::min(stop, safe_size - 1LL);
    return {static_cast<std::size_t>(start), static_cast<std::size_t>(stop), false};
}

}  // namespace

bool SortedSet::ScoreMemberLess::operator()(const std::pair<double, std::string>& left,
                                             const std::pair<double, std::string>& right) const
    noexcept {
    if (left.first < right.first) {
        return true;
    }
    if (right.first < left.first) {
        return false;
    }
    return left.second < right.second;
}

bool SortedSet::add(std::string member, double score_value) {
    const auto existing = scores_.find(member);
    if (existing != scores_.end()) {
        if (existing->second == score_value) {
            return false;
        }
        ordered_.erase({existing->second, existing->first});
        existing->second = score_value;
        ordered_.emplace(score_value, existing->first);
        return false;
    }
    const auto [iterator, inserted] = scores_.emplace(std::move(member), score_value);
    (void)inserted;
    ordered_.emplace(score_value, iterator->first);
    return true;
}

bool SortedSet::remove(std::string_view member) {
    const auto existing = scores_.find(std::string{member});
    if (existing == scores_.end()) {
        return false;
    }
    ordered_.erase({existing->second, existing->first});
    scores_.erase(existing);
    return true;
}

std::optional<double> SortedSet::score(std::string_view member) const {
    const auto existing = scores_.find(std::string{member});
    if (existing == scores_.end()) {
        return std::nullopt;
    }
    return existing->second;
}

std::size_t SortedSet::size() const noexcept { return scores_.size(); }

std::vector<std::pair<std::string, double>> SortedSet::range(long long start, long long stop) const {
    const auto range_value = normalize_range(start, stop, ordered_.size());
    std::vector<std::pair<std::string, double>> result;
    if (range_value.empty) {
        return result;
    }
    result.reserve(range_value.end - range_value.begin + 1U);
    auto iterator = ordered_.begin();
    std::advance(iterator, static_cast<std::ptrdiff_t>(range_value.begin));
    for (std::size_t index = range_value.begin; index <= range_value.end; ++index, ++iterator) {
        result.emplace_back(iterator->second, iterator->first);
    }
    return result;
}

}  // namespace keyforge::storage


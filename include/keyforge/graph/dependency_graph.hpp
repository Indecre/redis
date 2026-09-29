#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace keyforge::graph {

class DependencyGraph {
public:
    [[nodiscard]] bool add_dependencies(const std::string& child,
                                        const std::vector<std::string>& parents,
                                        std::string& error);
    [[nodiscard]] std::vector<std::string> parents(std::string_view key) const;
    [[nodiscard]] std::vector<std::string> dependents(std::string_view key) const;
    [[nodiscard]] std::vector<std::string> cascade(std::string_view root) const;
    void remove_key(std::string_view key);
    void clear();
    [[nodiscard]] std::size_t edge_count() const noexcept;

private:
    [[nodiscard]] bool reachable(std::string_view start, std::string_view target) const;

    std::unordered_map<std::string, std::unordered_set<std::string>> parents_;
    std::unordered_map<std::string, std::unordered_set<std::string>> dependents_;
    std::size_t edge_count_{0};
};

}  // namespace keyforge::graph


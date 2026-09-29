#include "keyforge/graph/dependency_graph.hpp"

#include <algorithm>
#include <deque>

namespace keyforge::graph {

bool DependencyGraph::add_dependencies(const std::string& child,
                                       const std::vector<std::string>& parents,
                                       std::string& error) {
    std::unordered_set<std::string> unique;
    for (const auto& parent : parents) {
        if (parent == child) {
            error = "dependency would create a self-cycle";
            return false;
        }
        if (!unique.insert(parent).second) {
            continue;
        }
        if (reachable(child, parent)) {
            error = "dependency would create a cycle";
            return false;
        }
    }
    for (const auto& parent : unique) {
        if (parents_[child].insert(parent).second) {
            dependents_[parent].insert(child);
            ++edge_count_;
        }
    }
    return true;
}

std::vector<std::string> DependencyGraph::parents(std::string_view key) const {
    std::vector<std::string> result;
    const auto iterator = parents_.find(std::string{key});
    if (iterator != parents_.end()) {
        result.assign(iterator->second.begin(), iterator->second.end());
        std::sort(result.begin(), result.end());
    }
    return result;
}

std::vector<std::string> DependencyGraph::dependents(std::string_view key) const {
    std::vector<std::string> result;
    const auto iterator = dependents_.find(std::string{key});
    if (iterator != dependents_.end()) {
        result.assign(iterator->second.begin(), iterator->second.end());
        std::sort(result.begin(), result.end());
    }
    return result;
}

std::vector<std::string> DependencyGraph::cascade(std::string_view root) const {
    std::vector<std::string> result;
    std::deque<std::string> pending;
    std::unordered_set<std::string> visited;
    pending.emplace_back(root);
    visited.emplace(root);
    while (!pending.empty()) {
        std::string current = std::move(pending.front());
        pending.pop_front();
        result.push_back(current);
        auto children = dependents(current);
        for (auto& child : children) {
            if (visited.insert(child).second) {
                pending.push_back(std::move(child));
            }
        }
    }
    return result;
}

void DependencyGraph::remove_key(std::string_view key) {
    const std::string owned_key{key};
    const auto parent_iterator = parents_.find(owned_key);
    if (parent_iterator != parents_.end()) {
        for (const auto& parent : parent_iterator->second) {
            auto reverse = dependents_.find(parent);
            if (reverse != dependents_.end()) {
                edge_count_ -= reverse->second.erase(owned_key);
                if (reverse->second.empty()) {
                    dependents_.erase(reverse);
                }
            }
        }
        parents_.erase(parent_iterator);
    }
    const auto dependent_iterator = dependents_.find(owned_key);
    if (dependent_iterator != dependents_.end()) {
        for (const auto& child : dependent_iterator->second) {
            auto reverse = parents_.find(child);
            if (reverse != parents_.end()) {
                edge_count_ -= reverse->second.erase(owned_key);
                if (reverse->second.empty()) {
                    parents_.erase(reverse);
                }
            }
        }
        dependents_.erase(dependent_iterator);
    }
}

void DependencyGraph::clear() {
    parents_.clear();
    dependents_.clear();
    edge_count_ = 0;
}

std::size_t DependencyGraph::edge_count() const noexcept { return edge_count_; }

bool DependencyGraph::reachable(std::string_view start, std::string_view target) const {
    std::vector<std::string> pending{std::string{start}};
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        std::string current = std::move(pending.back());
        pending.pop_back();
        if (current == target) {
            return true;
        }
        if (!visited.insert(current).second) {
            continue;
        }
        const auto iterator = dependents_.find(current);
        if (iterator != dependents_.end()) {
            pending.insert(pending.end(), iterator->second.begin(), iterator->second.end());
        }
    }
    return false;
}

}  // namespace keyforge::graph

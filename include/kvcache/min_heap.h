#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kvcache {

// Indexed binary heap. Smaller Compare priority wins, then smaller key.
// Compare must not throw. Intended values are small eviction ranks/timestamps.
template <class T, class Compare = std::less<T>>
class MinHeap {
    static_assert(std::is_nothrow_move_constructible<T>::value &&
                  std::is_nothrow_move_assignable<T>::value,
                  "heap priorities must support non-throwing moves");
public:
    struct Entry { std::size_t key; T value; };
    explicit MinHeap(Compare compare = Compare()) : compare_(std::move(compare)) {}
    bool empty() const noexcept { return data_.empty(); }
    std::size_t size() const noexcept { return data_.size(); }
    bool contains(std::size_t key) const { return positions_.count(key) != 0; }

    const Entry& top() const {
        if (empty()) throw std::out_of_range("heap is empty");
        return data_.front();
    }

    void push(std::size_t key, const T& value) {
        if (contains(key)) throw std::invalid_argument("duplicate heap key");
        data_.push_back({key, value});
        try {
            positions_.emplace(key, data_.size() - 1);
        } catch (...) {
            data_.pop_back();
            throw;
        }
        up(data_.size() - 1);
    }

    bool update(std::size_t key, const T& value) {
        auto it = positions_.find(key);
        if (it == positions_.end()) return false;
        T replacement = value;
        auto index = it->second;
        data_[index].value = std::move(replacement);
        fix(index);
        return true;
    }

    bool erase(std::size_t key) {
        auto it = positions_.find(key);
        if (it == positions_.end()) return false;
        auto index = it->second;
        swap_entries(index, data_.size() - 1);
        positions_.erase(key);
        data_.pop_back();
        if (index < data_.size()) fix(index);
        return true;
    }

    Entry pop() {
        Entry result = top();
        erase(result.key);
        return result;
    }

    bool check_invariants() const {
        if (data_.size() != positions_.size()) return false;
        for (std::size_t i = 0; i < data_.size(); ++i) {
            auto it = positions_.find(data_[i].key);
            if (it == positions_.end() || it->second != i) return false;
            if (i != 0 && before(data_[i], data_[(i - 1) / 2])) return false;
        }
        return true;
    }

private:
    bool before(const Entry& a, const Entry& b) const {
        if (compare_(a.value, b.value)) return true;
        if (compare_(b.value, a.value)) return false;
        return a.key < b.key;
    }
    void swap_entries(std::size_t a, std::size_t b) {
        if (a == b) return;
        std::swap(data_[a], data_[b]);
        positions_.at(data_[a].key) = a;
        positions_.at(data_[b].key) = b;
    }
    void up(std::size_t index) {
        while (index != 0) {
            auto parent = (index - 1) / 2;
            if (!before(data_[index], data_[parent])) break;
            swap_entries(index, parent);
            index = parent;
        }
    }
    void down(std::size_t index) {
        while (index < data_.size() / 2) {
            auto child = 2 * index + 1;
            if (child + 1 < data_.size() && before(data_[child + 1], data_[child])) ++child;
            if (!before(data_[child], data_[index])) break;
            swap_entries(index, child);
            index = child;
        }
    }
    void fix(std::size_t index) {
        if (index != 0 && before(data_[index], data_[(index - 1) / 2])) up(index);
        else down(index);
    }

    Compare compare_;
    std::vector<Entry> data_;
    std::unordered_map<std::size_t, std::size_t> positions_;
};

} // namespace kvcache

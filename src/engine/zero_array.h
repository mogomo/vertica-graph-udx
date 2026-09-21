// vgraph engine: an array that starts as zeros without being written.
// A std::vector writes every element when it is created. For a search that visits 50 nodes of
// a graph with 100 million, that write is most of the run time. calloc takes a large block
// straight from the operating system, which hands out zero pages only when they are touched.
#ifndef VGRAPH_ENGINE_ZERO_ARRAY_H
#define VGRAPH_ENGINE_ZERO_ARRAY_H

#include <cstddef>
#include <cstdlib>
#include <new>

namespace vgraph {

template <class T>
class ZeroArray {
public:
    ZeroArray() = default;
    ZeroArray(const ZeroArray &) = delete;
    ZeroArray &operator=(const ZeroArray &) = delete;
    ~ZeroArray() { std::free(data_); }

    // At least n elements. Growing drops the content: call it only while all elements are zero.
    void ensure(std::size_t n) {
        if (n <= size_) return;
        std::free(data_);
        data_ = nullptr; size_ = 0;
        data_ = static_cast<T *>(std::calloc(n, sizeof(T)));
        if (!data_) throw std::bad_alloc();
        size_ = n;
    }
    // All zero again, without writing: a fresh block.
    void clear() {
        const std::size_t n = size_;
        std::free(data_);
        data_ = nullptr; size_ = 0;
        ensure(n);
    }
    // Exactly n zero elements in a fresh block: also makes the array smaller.
    void renew(std::size_t n) {
        std::free(data_);
        data_ = nullptr; size_ = 0;
        ensure(n);
    }
    void swap(ZeroArray &other) {
        T *d = data_; data_ = other.data_; other.data_ = d;
        std::size_t n = size_; size_ = other.size_; other.size_ = n;
    }
    std::size_t size() const { return size_; }
    T &operator[](std::size_t i) { return data_[i]; }
    const T &operator[](std::size_t i) const { return data_[i]; }

private:
    T *data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace vgraph

#endif

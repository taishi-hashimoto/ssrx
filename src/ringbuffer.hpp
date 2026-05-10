#ifndef SSRX_RINGBUFFER_HPP
#define SSRX_RINGBUFFER_HPP

#include <atomic>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ssrx {
    

class RingBufferError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};


/// @brief Ring buffer class.
template<typename T>
class RingBuffer {

public:
    class Block;
    typedef T value_type;

public:

    /// @brief Create an uninitialized ringbuffer.
    RingBuffer() 
        : blocks()
        , write_pointer_(0)
        , read_pointer_(0)
    { }

    /// @brief Initializes a ring buffer.
    /// @param min_numblk The minimum number of blocks on the ring buffer. Actual number of blocks is automatically computed to be power of two.
    /// @param args Constructor arguments for buffer data.
    template<typename... Args>
    RingBuffer(size_t min_numblk, Args&&... args)
        : blocks()
        , write_pointer_(0)
        , read_pointer_(0)
    {
        reset(min_numblk, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void reset(size_t min_numblk, Args&&... args) {
        auto exponent = min_numblk > 1
            ? static_cast<std::size_t>(std::ceil(std::log2(min_numblk)))
            : 0u;
        auto nblocks = size_t(1) << exponent;
        blocks.clear();
        blocks.reserve(nblocks);
        for (size_t i = 0; i < nblocks; ++i) {
            blocks.emplace_back(args...);
        }
        write_pointer_ = 0;
        read_pointer_ = 0;
    }

    RingBuffer(const RingBuffer&) = delete;

    virtual ~RingBuffer() { }

    /// @brief Current write pointer (readonly).
    size_t write_pointer() const {
        return write_pointer_;
    }

    /// @brief Current read pointer (readonly).
    size_t read_pointer() const {
        return read_pointer_;
    }

    /// @return Message buffer at the current `write_pointer`.
    Block& write_buffer() {
        return blocks[write_pointer_];
    }

    /// @return Message buffer at the current `read_pointer`.
    Block& read_buffer() {
        return blocks[read_pointer_];
    }

    Block& operator [](size_t index) {
        return blocks[index & (size() - 1)];
    }

    const Block& operator [](size_t index) const {
        return blocks[index & (size() - 1)];
    }

    /// @brief Advances the `write_pointer`.
    void advance_write_pointer() {
        // Finalize the write buffer.
        write_buffer().set_state_readable();
        // Advance write pointer.
        write_pointer_ += 1;
        write_pointer_ &= (size() - 1);
    }

    /// @brief Advances the `read_pointer`.
    void advance_read_pointer() {
        // Finalize the read buffer.
        read_buffer().set_state_writable();
        // Advance read pointer.
        read_pointer_ += 1;
        read_pointer_ &= (size() - 1);
    }

    /// @brief Check if the current write buffer is ready to write.
    void check_writable() {
        if (!write_buffer().is_writable()) {
            throw RingBufferError("Writing too fast");
        }
    }

    /// @brief Check if the current read buffer is ready to read.
    void check_readable() {
        if (!read_buffer().is_readable()) {
            throw RingBufferError("Reading too fast");
        }
    }

    /// @brief Actual size of the ringbuffer.
    size_t size() const {
        return blocks.size();
    }

private:

    /// @brief Data blocks.
    std::vector<Block> blocks;

    /// @brief Write pointer.
    size_t write_pointer_;

    /// @brief Read pointer.
    size_t read_pointer_;

public:

    /// @brief Each block of the ring buffer.
    class Block {

    public:
        /// @brief Initializes each block.
        /// @param args Constructor arguments for actual data content.
        template<typename... Args>
        Block(Args&&... args)
            : data(args...)
            , writable(true)
        { }

        Block(Block&& other)
            : data(std::move(other.data))
            , writable(other.writable.load())
        { }

        Block(const Block&) = delete;

        virtual ~Block() { }

        /// @return Buffer content.
        T& content() {
            return data;
        }

        /// @return Buffer content.
        const T& content() const {
            return data;
        }

        /// @brief Set block state to writable.
        void set_state_writable() {
            writable.store(true, std::memory_order_release);
        }

        /// @brief Set block state to readable.
        void set_state_readable() {
            writable.store(false, std::memory_order_release);
        }

        /// @brief Return true if block state is writable.
        bool is_writable() const {
            return writable.load(std::memory_order_acquire);
        }

        /// @brief Return true if block state is readable. 
        bool is_readable() const {
            return !is_writable();
        }

    private:

        /// @brief Data content.
        T data;

        /// @brief Indicates whether the buffer is writable or readable.
        std::atomic<bool> writable;
    };
};

}

#endif

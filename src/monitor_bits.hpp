#ifndef SSRX_MONITOR_BITS_HPP
#define SSRX_MONITOR_BITS_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ssrx { namespace monitor_bits {

inline size_t packed_bytes(size_t nbits) {
    return (nbits + 7) / 8;
}

template<typename BitAt>
std::vector<uint8_t> pack(size_t nbits, BitAt bit_at) {
    std::vector<uint8_t> data(sizeof(size_t) + packed_bytes(nbits), 0);
    std::memcpy(data.data(), &nbits, sizeof(nbits));

    auto* bytes = data.data() + sizeof(nbits);
    for (size_t i = 0; i < nbits; ++i) {
        if (bit_at(i)) {
            bytes[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }

    return data;
}

inline bool test(const uint8_t* bytes, size_t nbytes, size_t index) {
    auto byte_index = index / 8;
    if (byte_index >= nbytes) {
        return false;
    }
    return (bytes[byte_index] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

inline size_t count(const uint8_t* bytes, size_t nbytes, size_t nbits) {
    size_t nset = 0;
    for (size_t i = 0; i < nbits; ++i) {
        if (test(bytes, nbytes, i)) {
            ++nset;
        }
    }
    return nset;
}

} }

#endif

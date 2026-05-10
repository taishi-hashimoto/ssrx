#include "modes.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

template<typename T, typename U>
void expect_equal(std::string_view name, const T& actual, const U& expected)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << name << ": expected " << expected << ", got " << actual << '\n';
        ++failures;
    }
}

void expect_true(std::string_view name, bool value)
{
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

unsigned char hex_value(char c)
{
    if ('0' <= c && c <= '9') {
        return static_cast<unsigned char>(c - '0');
    }
    if ('a' <= c && c <= 'f') {
        return static_cast<unsigned char>(c - 'a' + 10);
    }
    if ('A' <= c && c <= 'F') {
        return static_cast<unsigned char>(c - 'A' + 10);
    }
    throw std::runtime_error("invalid hex character");
}

std::vector<unsigned char> parse_hex(std::string_view text)
{
    if (text.size() % 2 != 0) {
        throw std::runtime_error("hex string must have an even length");
    }

    std::vector<unsigned char> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        bytes.push_back(static_cast<unsigned char>((hex_value(text[i]) << 4) | hex_value(text[i + 1])));
    }
    return bytes;
}

uint32_t parity_field(const std::vector<unsigned char>& msg)
{
    const auto n = msg.size();
    return (static_cast<uint32_t>(msg[n - 3]) << 16)
        | (static_cast<uint32_t>(msg[n - 2]) << 8)
        | static_cast<uint32_t>(msg[n - 1]);
}

void flip_bit(std::vector<unsigned char>& msg, int bit_index)
{
    msg[bit_index / 8] ^= static_cast<unsigned char>(1 << (7 - (bit_index % 8)));
}

void test_message_length()
{
    for (int df : {0, 4, 5, 11}) {
        expect_equal("short message length", modesMessageLenByType(df), 56);
    }

    for (int df : {16, 17, 18, 19, 20, 21, 24}) {
        expect_equal("long message length", modesMessageLenByType(df), 112);
    }
}

void test_crc()
{
    auto msg = parse_hex("8D4840D6202CC371C32CE0576098");
    expect_equal("fixture parity", parity_field(msg), 0x576098u);
    expect_equal("DF17 CRC", modesChecksum(msg.data(), 112), 0x576098u);
}

void test_single_bit_correction()
{
    const auto original = parse_hex("8D4840D6202CC371C32CE0576098");
    auto corrupted = original;
    flip_bit(corrupted, 20);

    expect_true("single-bit fixture was corrupted", corrupted != original);
    expect_equal("single-bit correction index", fixSingleBitErrors(corrupted.data(), 112), 20);
    expect_true("single-bit correction restores message", corrupted == original);
}

void test_bin2hex()
{
    const std::vector<uint8_t> bytes {0x00, 0x0a, 0x10, 0xff};
    expect_equal("lowercase hex", ssrx::bin2hex(bytes.data(), bytes.size()), std::string("000a10ff"));
    expect_equal("uppercase hex", ssrx::bin2hex(bytes.data(), bytes.size(), true), std::string("000A10FF"));
}

void test_aircraft_cache_store_load()
{
    const auto root = std::filesystem::temp_directory_path()
        / ("ssrx-test-modes-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto db_path = root / "aircraft.sqlite3";

    std::filesystem::remove_all(root);

    const auto first_seen = std::chrono::system_clock::from_time_t(1700000000);
    const auto second_seen = std::chrono::system_clock::from_time_t(1700000123);

    {
        ssrx::ModesDecoder decoder(db_path.string());
        decoder.addresses[0xABCDEF] = ssrx::ModesDecoder::Aircraft { first_seen };
        decoder.addresses[0x123456] = ssrx::ModesDecoder::Aircraft { second_seen };
        decoder.store();
    }

    {
        ssrx::ModesDecoder loaded(db_path.string());
        expect_equal("loaded aircraft count", loaded.addresses.size(), size_t {2});
        expect_true("loaded first aircraft", loaded.addresses.contains(0xABCDEF));
        expect_true("loaded second aircraft", loaded.addresses.contains(0x123456));
        expect_equal(
            "loaded first timestamp",
            std::chrono::system_clock::to_time_t(loaded.addresses.at(0xABCDEF).last_seen),
            std::chrono::system_clock::to_time_t(first_seen));
        expect_equal(
            "loaded second timestamp",
            std::chrono::system_clock::to_time_t(loaded.addresses.at(0x123456).last_seen),
            std::chrono::system_clock::to_time_t(second_seen));
    }

    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    test_message_length();
    test_crc();
    test_single_bit_correction();
    test_bin2hex();
    test_aircraft_cache_store_load();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }

    return 0;
}

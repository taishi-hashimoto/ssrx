#include <algorithm>
#include <complex>
#include <cstring>
#include <fftw3.h>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <span>
#include <vector>
// #include "sqlite3.h"
#include <sqlite3.h>
#include "modes.hpp"


constexpr size_t MODES_LONG_MSG_BITS = 112;
constexpr size_t MODES_LONG_MSG_BYTES = MODES_LONG_MSG_BITS / 8;

std::vector<std::complex<float>> ssrx::modes::make_modes_preamble(
    size_t preamble_size,
    double sampling_rate)
{
    std::vector<std::complex<float>> preamble(preamble_size);

    std::vector<std::pair<double, double>> preamble_high_pos {
        {0., 0.5e-6}, {1e-6, 1.5e-6}, {3.5e-6, 4e-6}, {4.5e-6, 5e-6}
    };

    for (const auto& span : preamble_high_pos) {
        auto beg = static_cast<size_t>(span.first * sampling_rate);
        auto end = static_cast<size_t>(span.second * sampling_rate);
        for (auto cur = beg; cur < end && cur < preamble.size(); ++cur) {
            preamble[cur] = std::complex<float>(1.f, 0.f);
        }
    }

    return preamble;
}

void ssrx::modes::print_preamble(
    std::span<const std::complex<float>> preamble,
    std::ostream& os)
{
    for (const auto& v : preamble) {
        os << (v == std::complex<float>() ? '_' : '|');
    }
}

void ssrx::modes::compute_preamble_fft(
    std::span<const std::complex<float>> preamble,
    std::span<std::complex<float>> spectrum)
{
    std::fill(spectrum.begin(), spectrum.end(), std::complex<float>());
    auto ncopy = std::min(preamble.size(), spectrum.size());
    std::copy_n(preamble.begin(), ncopy, spectrum.begin());

    auto plan = fftwf_plan_dft_1d(
        static_cast<int>(spectrum.size()),
        reinterpret_cast<fftwf_complex*>(spectrum.data()),
        reinterpret_cast<fftwf_complex*>(spectrum.data()),
        FFTW_FORWARD,
        FFTW_MEASURE | FFTW_UNALIGNED);

    fftwf_execute(plan);
    fftwf_destroy_plan(plan);
}


ssrx::ModesDecoder::ModesDecoder(const std::string& path_sqlite3)
    : path_sqlite3(path_sqlite3)
{
    if (!std::filesystem::exists(path_sqlite3)) {
        return;
    }

    sqlite3 *db;
    if (sqlite3_open(path_sqlite3.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("sqlite3_open()");
    }

    sqlite3_stmt *stmt = NULL;
    const char * sql =
        "SELECT address, last_seen FROM aircraft";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        auto address = sqlite3_column_int(stmt, 0);
        auto last_seen = sqlite3_column_int64(stmt, 1);
        addresses[address] = Aircraft { std::chrono::system_clock::from_time_t(last_seen) };
    }
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

ssrx::ModesDecoder::~ModesDecoder()
{
    try {
        store();
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
    }
}

void ssrx::ModesDecoder::store() {
    // Save aircrafts dict.
    std::filesystem::create_directories(std::filesystem::path(path_sqlite3).parent_path());

    sqlite3 *db;
    if (sqlite3_open(path_sqlite3.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("sqlite3_open()");
    }
    sqlite3_exec(
        db,
        "CREATE TABLE IF NOT EXISTS aircraft("
        "  address INTEGER PRIMARY KEY,"
        "  last_seen INTEGER"
        ");"
        "BEGIN TRANSACTION;",
        NULL, NULL, NULL);
    sqlite3_stmt *stmt = NULL;
    const char * sql =
        "INSERT INTO aircraft(address, last_seen)"
        "VALUES(?, ?)"
        "ON CONFLICT(address) DO UPDATE"
        "  SET last_seen = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    for (auto& aircraft : addresses) {
        auto address = aircraft.first;
        auto last_seen = std::chrono::system_clock::to_time_t(aircraft.second.last_seen);
        sqlite3_bind_int(stmt, 1, address);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(last_seen));
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(last_seen));
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
        if (sqlite3_reset(stmt) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void ssrx::ModesDecoder::add_aircraft(uint32_t address) {
    addresses[address] = Aircraft { std::chrono::system_clock::now() };
}

std::string ssrx::bin2hex(const uint8_t* data, size_t length, bool uppercase) {
    std::ostringstream oss;
    for (size_t i = 0; i < length; ++i) {
        oss << (uppercase ? std::uppercase : std::nouppercase)
            << std::setw(2)
            << std::setfill('0')
            << std::hex
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

uint32_t ssrx::ModesDecoder::test_crc(uint8_t* msg, int df, int nbits) {
    unsigned char aux[MODES_LONG_MSG_BYTES];
    int msgtype = df;
    int msgbits = nbits;

    if (msgtype == 0 ||         /* Short air surveillance */
        msgtype == 4 ||         /* Surveillance, altitude reply */
        msgtype == 5 ||         /* Surveillance, identity reply */
        msgtype == 16 ||        /* Long Air-Air survillance */
        msgtype == 20 ||        /* Comm-A, altitude request */
        msgtype == 21 ||        /* Comm-A, identity request */
        msgtype == 24)          /* Comm-C ELM */
    {
        uint32_t addr;
        uint32_t crc;
        int lastbyte = (msgbits/8)-1;

        /* Work on a copy. */
        memcpy(aux,msg,msgbits/8);

        /* Compute the CRC of the message and XOR it with the AP field
         * so that we recover the address, because:
         *
         * (ADDR xor CRC) xor CRC = ADDR. */
        crc = modesChecksum(aux,msgbits);
        aux[lastbyte] ^= crc & 0xff;
        aux[lastbyte-1] ^= (crc >> 8) & 0xff;
        aux[lastbyte-2] ^= (crc >> 16) & 0xff;
        
        /* If the obtained address exists in our cache we consider
         * the message valid. */
        addr = aux[lastbyte] | (aux[lastbyte-1] << 8) | (aux[lastbyte-2] << 16);
        if (addresses.find(addr) != addresses.end()) {
            return addr;
        }
    }
    return 0;
}

int modesMessageLenByType(int type) {
    switch (type) {
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 24:
        return 112;
    default:
        return 56;
    }
}

/* Parity table for MODE S Messages.
 * The table contains 112 elements, every element corresponds to a bit set
 * in the message, starting from the first bit of actual data after the
 * preamble.
 *
 * For messages of 112 bit, the whole table is used.
 * For messages of 56 bits only the last 56 elements are used.
 *
 * The algorithm is as simple as xoring all the elements in this table
 * for which the corresponding bit on the message is set to 1.
 *
 * The latest 24 elements in this table are set to 0 as the checksum at the
 * end of the message should not affect the computation.
 *
 * Note: this function can be used with DF11 and DF17, other modes have
 * the CRC xored with the sender address as they are reply to interrogations,
 * but a casual listener can't split the address from the checksum.
 */
static uint32_t modes_checksum_table[112] = {
    0x3935ea, 0x1c9af5, 0xf1b77e, 0x78dbbf, 0xc397db, 0x9e31e9, 0xb0e2f0, 0x587178,
    0x2c38bc, 0x161c5e, 0x0b0e2f, 0xfa7d13, 0x82c48d, 0xbe9842, 0x5f4c21, 0xd05c14,
    0x682e0a, 0x341705, 0xe5f186, 0x72f8c3, 0xc68665, 0x9cb936, 0x4e5c9b, 0xd8d449,
    0x939020, 0x49c810, 0x24e408, 0x127204, 0x093902, 0x049c81, 0xfdb444, 0x7eda22,
    0x3f6d11, 0xe04c8c, 0x702646, 0x381323, 0xe3f395, 0x8e03ce, 0x4701e7, 0xdc7af7,
    0x91c77f, 0xb719bb, 0xa476d9, 0xadc168, 0x56e0b4, 0x2b705a, 0x15b82d, 0xf52612,
    0x7a9309, 0xc2b380, 0x6159c0, 0x30ace0, 0x185670, 0x0c2b38, 0x06159c, 0x030ace,
    0x018567, 0xff38b7, 0x80665f, 0xbfc92b, 0xa01e91, 0xaff54c, 0x57faa6, 0x2bfd53,
    0xea04ad, 0x8af852, 0x457c29, 0xdd4410, 0x6ea208, 0x375104, 0x1ba882, 0x0dd441,
    0xf91024, 0x7c8812, 0x3e4409, 0xe0d800, 0x706c00, 0x383600, 0x1c1b00, 0x0e0d80,
    0x0706c0, 0x038360, 0x01c1b0, 0x00e0d8, 0x00706c, 0x003836, 0x001c1b, 0xfff409,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000
};

uint32_t modesChecksum(unsigned char *msg, int bits) {
    uint32_t crc = 0;
    int offset = (bits == 112) ? 0 : (112-56);
    int j;

    for(j = 0; j < bits; j++) {
        int byte = j/8;
        int bit = j%8;
        int bitmask = 1 << (7-bit);

        /* If bit is set, xor with corresponding table entry. */
        if (msg[byte] & bitmask)
            crc ^= modes_checksum_table[j+offset];
    }
    return crc; /* 24 bit checksum. */
}

/* Try to fix single bit errors using the checksum. On success modifies
* the original buffer with the fixed version, and returns the position
* of the error bit. Otherwise if fixing failed -1 is returned. */
int fixSingleBitErrors(unsigned char *msg, int bits) {
    int j;
    unsigned char aux[112/8];

    for (j = 0; j < bits; j++) {
        int byte = j/8;
        int bitmask = 1 << (7-(j%8));
        uint32_t crc1, crc2;

        memcpy(aux,msg,bits/8);
        aux[byte] ^= bitmask; /* Flip j-th bit. */

        crc1 = ((uint32_t)aux[(bits/8)-3] << 16) |
            ((uint32_t)aux[(bits/8)-2] << 8) |
                (uint32_t)aux[(bits/8)-1];
        crc2 = modesChecksum(aux,bits);

        if (crc1 == crc2) {
            /* The error is fixed. Overwrite the original buffer with
            * the corrected sequence, and returns the error bit
            * position. */
            memcpy(msg,aux,bits/8);
            return j;
        }
    }
    return -1;
}

#ifndef SSRX_MODES_HPP
#define SSRX_MODES_HPP

#include <complex>
#include <cstddef>
#include <iosfwd>
#include <span>
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>


namespace ssrx {
    namespace modes {
        std::vector<std::complex<float>> make_modes_preamble(
            size_t preamble_size,
            double sampling_rate);

        void print_preamble(
            std::span<const std::complex<float>> preamble,
            std::ostream& os);

        void compute_preamble_fft(
            std::span<const std::complex<float>> preamble,
            std::span<std::complex<float>> spectrum);
    }

    /// @brief Mode-S decoder. Mostly based on dump1090.
    class ModesDecoder {
    public:
        /// @brief Constructor.
        /// @param path_sqlite3 Path to SQLite3 database file for storing addresses of aircrafts.
        ModesDecoder(const std::string& path_sqlite3);

        /// @brief Destructor.
        /// @note `addresses` will be written into `path_sqlite3`.
        ~ModesDecoder();

        /// @brief Update ICAO addresses database on the disk.
        void store();

        /// @brief Add an aircraft.
        /// @param address Aircraft address.
        void add_aircraft(uint32_t address);

        /// @brief Check if the CRC part of the given message is xor of actual CRC and one of aircraft addresses ever seen.
        /// @param msg Mode-S message candidate.
        /// @param df Downlink format.
        /// @param nbits No. bits in messages.
        /// @return Found address, or 0 if none found.
        uint32_t test_crc(uint8_t* msg, int df, int nbits);

        /// @brief Structure for the aircraft address.
        struct Aircraft {
            /// @brief Time when this aircraft was last seen.
            std::chrono::system_clock::time_point last_seen;
        };

        /// @brief Address database.
        std::string path_sqlite3;

        /// @brief Addresses received so far.
        std::unordered_map<uint32_t, Aircraft> addresses;
    };
    
    /// @brief Convert uint8_t message into hex format.
    /// @param data 
    /// @param length 
    /// @return 
    std::string bin2hex(const uint8_t* data, size_t length, bool uppercase = false);
}

/* Given the Downlink Format (DF) of the message, return the message length
 * in bits. */
int modesMessageLenByType(int type);


uint32_t modesChecksum(unsigned char *msg, int bits);

/* Try to fix single bit errors using the checksum. On success modifies
* the original buffer with the fixed version, and returns the position
* of the error bit. Otherwise if fixing failed -1 is returned. */
int fixSingleBitErrors(unsigned char *msg, int bits);

#endif

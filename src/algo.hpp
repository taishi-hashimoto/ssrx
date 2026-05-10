#ifndef SSRX_ALGO_HPP
#define SSRX_ALGO_HPP

#include <algorithm>
#include <complex>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "sdr.hpp"
#include "modes.hpp"

namespace ssrx { namespace algo {

struct Detection {
    size_t index;
    double high;
    double low;
};

struct Message {
    Detection detection;
    sdr::TimeSpec time;
    std::string text;
    std::vector<std::complex<float>> samples;
    double rssi;
};

void correlate_frequency_domain(
    std::span<std::complex<float>> spectrum,
    std::span<const std::complex<float>> preamble_fft);

void normalize_correlation_power(
    std::span<std::complex<float>> correlation,
    float fft_size);

std::vector<Detection> detect_preamble(
    std::span<const std::complex<float>> samples,
    std::span<const std::complex<float>> correlation,
    size_t index_begin,
    size_t index_end,
    size_t ncoh,
    double snr);

void hard_demodulate_bits(
    std::span<const double> integrated_power,
    size_t nskip,
    std::span<int> bits);

std::vector<Message> demodulate(
    std::span<const Detection> detections,
    std::span<const std::complex<float>> samples,
    const sdr::TimeSpec& time,
    size_t nsamples_zero_padding,
    size_t nsamples_preamble,
    size_t nsamples_data_long,
    size_t ncoh,
    double sampling_rate,
    ModesDecoder& modes);

} }

#endif

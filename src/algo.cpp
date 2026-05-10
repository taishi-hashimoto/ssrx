#include "algo.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <sstream>

namespace {

void integrate_coherent_power(
    std::span<const std::complex<float>> samples,
    std::span<double> output,
    size_t ncoh)
{
    if (ncoh < 2) {
        throw std::runtime_error("The number of coherent integration must be larger than 1.");
    }

    const auto required_outputs = (samples.size() + ncoh - 1) / ncoh;
    if (output.size() < required_outputs) {
        throw std::runtime_error("Coherent integration output buffer is too small.");
    }

    size_t sample_index = 0;
    size_t output_index = 0;
    while (sample_index < samples.size()) {
        auto sum = samples[sample_index++];
        for (size_t i = 1; i < ncoh && sample_index < samples.size(); ++i) {
            sum += samples[sample_index++];
        }
        output[output_index++] = std::norm(sum / static_cast<float>(ncoh));
    }
}

}

namespace ssrx { namespace algo {

void correlate_frequency_domain(
    std::span<std::complex<float>> spectrum,
    std::span<const std::complex<float>> preamble_fft)
{
    auto n = std::min(spectrum.size(), preamble_fft.size());
    for (size_t i = 0; i < n; ++i) {
        spectrum[i] *= std::conj(preamble_fft[i]);
    }
}

void normalize_correlation_power(
    std::span<std::complex<float>> correlation,
    float fft_size)
{
    for (auto& x : correlation) {
        x = std::norm(x / fft_size);
    }
}

std::vector<Detection> detect_preamble(
    std::span<const std::complex<float>> samples,
    std::span<const std::complex<float>> correlation,
    size_t index_begin,
    size_t index_end,
    size_t ncoh,
    double snr)
{
    std::vector<Detection> detections;
    detections.reserve(128);

    if (index_begin >= index_end || index_end > samples.size() || index_end > correlation.size()) {
        return detections;
    }

    for (size_t i = index_begin; i < index_end; ++i) {
        std::array<std::complex<float>, 16> integrated{};
        std::array<double, 16> m{};

        for (size_t j = 0; j < 16; ++j) {
            for (size_t k = 0; k < ncoh; ++k) {
                integrated[j] += samples[i + k + j * ncoh];
            }
        }

        std::transform(integrated.begin(), integrated.end(), m.begin(),
            [](const std::complex<float>& x) { return std::norm(x); });

        if (!(
            m[0] > m[1] &&
            m[1] < m[2] &&
            m[2] > m[3] &&
            m[3] < m[0] &&
            m[4] < m[0] &&
            m[5] < m[0] &&
            m[6] < m[0] &&
            m[7] > m[8] &&
            m[8] < m[9] &&
            m[9] > m[6]
        )) {
            continue;
        }

        auto high = (m[0] + m[2] + m[7] + m[9]) / 4;
        auto low = (m[4] + m[5] + m[11] + m[12] + m[13] + m[14]) / 6;

        if (m[4] >= high || m[5] >= high) {
            continue;
        }

        if (m[11] >= high || m[12] >= high || m[13] >= high || m[14] >= high) {
            continue;
        }

        auto is_high_enough = high > low * snr;
        auto cc_peak = correlation[i].real();
        auto left = i > ncoh / 2 ? i - ncoh / 2 : 0;
        auto right = std::min(i + ncoh / 2, correlation.size());

        bool is_significant = std::all_of(
            correlation.begin() + left,
            correlation.begin() + right,
            [cc_peak](const auto& x) { return x.real() <= cc_peak; });

        if (!is_high_enough || !is_significant) {
            continue;
        }

        detections.push_back(Detection{ i, high, low });
    }

    std::sort(
        detections.begin(),
        detections.end(),
        [](const auto& a, const auto& b) { return a.high > b.high; });

    std::vector<Detection> filtered;
    filtered.reserve(detections.size());
    const size_t min_separation = ncoh;
    for (const auto& candidate : detections) {
        bool too_close = false;
        for (const auto& kept : filtered) {
            auto distance = candidate.index > kept.index
                ? candidate.index - kept.index
                : kept.index - candidate.index;
            if (distance <= min_separation) {
                too_close = true;
                break;
            }
        }
        if (!too_close) {
            filtered.push_back(candidate);
        }
    }

    std::sort(
        filtered.begin(),
        filtered.end(),
        [](const auto& a, const auto& b) { return a.index < b.index; });

    return filtered;
}

void hard_demodulate_bits(
    std::span<const double> integrated_power,
    size_t nskip,
    std::span<int> bits)
{
    for (size_t j = nskip; j + 1 < integrated_power.size(); j += 2) {
        size_t k = (j - nskip) / 2;
        if (k >= bits.size()) {
            break;
        }
        bits[k] = integrated_power[j] > integrated_power[j + 1] ? 1 : 0;
    }
}

std::vector<Message> demodulate(
    std::span<const Detection> detections,
    std::span<const std::complex<float>> samples,
    const sdr::TimeSpec& time,
    size_t nsamples_zero_padding,
    size_t nsamples_preamble,
    size_t nsamples_data_long,
    size_t ncoh,
    double sampling_rate,
    ModesDecoder& modes)
{
    std::vector<Message> messages;
    messages.reserve(detections.size());

    std::array<int, 112> bits{};
    std::array<uint8_t, 14> msg{};
    std::vector<std::complex<float>> raw(nsamples_preamble + nsamples_data_long);
    std::vector<double> power2((raw.size() + ncoh - 1) / ncoh);

    for (const auto& det : detections) {
        auto j = det.index;
        if (j + raw.size() > samples.size()) {
            continue;
        }

        auto now = time + (j - nsamples_zero_padding) / sampling_rate;
        std::copy_n(samples.data() + j, raw.size(), raw.begin());

        integrate_coherent_power(
            std::span<const std::complex<float>>(raw.data(), raw.size()),
            std::span<double>(power2.data(), power2.size()),
            ncoh);

        size_t nskip = nsamples_preamble / ncoh;
        hard_demodulate_bits(
            std::span<const double>(power2.data(), power2.size()),
            nskip,
            std::span<int>(bits.data(), bits.size()));

        for (size_t bit_index = 0; bit_index < bits.size(); bit_index += 8) {
            msg[bit_index / 8] =
                bits[bit_index + 0] << 7 |
                bits[bit_index + 1] << 6 |
                bits[bit_index + 2] << 5 |
                bits[bit_index + 3] << 4 |
                bits[bit_index + 4] << 3 |
                bits[bit_index + 5] << 2 |
                bits[bit_index + 6] << 1 |
                bits[bit_index + 7] << 0;
        }

        int msgtype = msg[0] >> 3;
        int msgbits = modesMessageLenByType(msgtype);
        int msglen = msgbits / 8;

        auto crc = ((uint32_t)msg[msglen - 3] << 16) |
                   ((uint32_t)msg[msglen - 2] << 8) |
                    (uint32_t)msg[msglen - 1];
        auto crc2 = modesChecksum(msg.data(), msgbits);
        if (crc != crc2 && (msgtype == 11 || msgtype == 17)) {
            if (fixSingleBitErrors(msg.data(), msgbits) != -1) {
                crc2 = modesChecksum(msg.data(), msgbits);
            }
        }

        uint32_t addr;
        int crc_ok = crc == crc2 ? 1 : 0;

        if (msgtype != 11 && msgtype != 17) {
            addr = modes.test_crc(msg.data(), msgtype, msgbits);
            if (addr) {
                crc_ok = 2;
                modes.add_aircraft(addr);
            }
        } else {
            if (crc_ok) {
                addr = (msg[1] << 16) | (msg[2] << 8) | msg[3];
                modes.add_aircraft(addr);
            }
        }

        if (crc_ok) {
            std::ostringstream oss;
            std::hex(oss);
            for (int i = 0; i < msglen; ++i) {
                oss << msg[i];
            }

            size_t nsamples_msg = nsamples_preamble / ncoh + msgbits * 2;
            double total_power = std::accumulate(
                power2.begin(), power2.begin() + nsamples_msg, 0.0);
            double rssi = 10 * std::log10(total_power / nsamples_msg);

            messages.push_back(Message{
                det,
                now,
                oss.str(),
                std::vector<std::complex<float>>(
                    raw.begin(), raw.begin() + nsamples_preamble + msgbits * 2 * ncoh),
                rssi
            });
        }
    }

    return messages;
}

} }

#ifndef SSRX_SSRX_HPP
#define SSRX_SSRX_HPP
#include <cmath>
#include <cstdint>
#include <zmq_addon.hpp>
#include "confighelper.hpp"
#include "algo.hpp"
#include "sdr.hpp"
#include "modes.hpp"


namespace ssrx {

inline double db_to_power_ratio(double db) {
    return std::pow(10.0, db / 10.0);
}

enum VType {
    VTYPE_C32,
    VTYPE_C16,
    VTYPE_C8,
};

struct Header {
    int64_t seconds;
    int64_t femtoseconds;
    double rssi;
    uint32_t msg_len;
    uint32_t ncoh;
    uint32_t nsymbols;
    uint32_t vtype;
};

inline Header build_header(
    const sdr::TimeSpec& time,
    double rssi,
    uint32_t msg_len,
    uint32_t ncoh,
    uint32_t nsymbols,
    VType vtype)
{
    return Header{
        time.seconds.count(),
        time.femtoseconds.count(),
        rssi,
        msg_len,
        ncoh,
        nsymbols,
        static_cast<uint32_t>(vtype)
    };
}

template<typename Base>
class rb_data : public Base {
public:
    using base_type = Base;
    using raw_type = typename Base::raw_type;
    using value_type = typename Base::value_type;

private:
    size_t nsamples_preamble;
    size_t nsamples_data_long;
    size_t ncoh;
    double sampling_rate;
    ModesDecoder& modes;

    size_t plan_raw2buf;
    size_t plan_buf2cc;

    using Detection = ssrx::algo::Detection;
    using Message = ssrx::algo::Message;

public:

    std::vector<Detection> detections;

    /// @brief Successfully demodulated messages in hex format.
    std::vector<Message> messages;

public:

    rb_data(
        size_t fft_size,
        size_t nsamples_preamble,
        size_t nsamples_data_long,
        size_t ncoh,
        double sampling_rate,
        ModesDecoder& modes
    )
        : base_type(
            fft_size,
            nsamples_preamble - 1,
            0,
            nsamples_preamble + nsamples_data_long,
            fft_size, 2)
        , nsamples_preamble(nsamples_preamble)
        , nsamples_data_long(nsamples_data_long)
        , ncoh(ncoh)
        , sampling_rate(sampling_rate)
        , modes(modes)
    {
        plan_raw2buf = this->plan(0, 1, FFTW_FORWARD);  // buffer[0] -> [1]: samples -> cc
        plan_buf2cc = this->plan(1, 1, FFTW_BACKWARD);  // buffer[1] -> [1]: cc -> cc
    }

    size_t nsamples_zero_padding() const {
        return nsamples_preamble - 1;
    }

    /// @brief Compute cross correlation.
    /// @param pp FFT of Mode-S preamble.
    value_type* correlate(const std::vector<value_type>& pp) {
        // Call convert method first.
        this->convert();

        // Perform FFT of the input signal.
        this->execute(plan_raw2buf);

        auto* cc = this->buffer(1);
        ssrx::algo::correlate_frequency_domain(
            std::span<value_type>(cc, this->fft_size()),
            std::span<const value_type>(pp.data(), pp.size()));
        this->execute(plan_buf2cc);

        ssrx::algo::normalize_correlation_power(
            std::span<value_type>(cc, this->fft_size()),
            static_cast<float>(this->fft_size()));
        return cc;
    }

    std::vector<Detection>& detect_preamble(double snr) {
        size_t index_beg = nsamples_zero_padding();
        size_t index_end = this->fft_size() - this->noverlapped();  // Last overlapped will be used for coherent integration.
        detections = ssrx::algo::detect_preamble(
            std::span<const value_type>(this->buffer(0), this->fft_size()),
            std::span<const value_type>(this->buffer(1), this->fft_size()),
            index_beg,
            index_end,
            ncoh,
            snr);
        
        return detections;
    }

    /// @brief Demodulate Mode-S from each detection index.
    void demodulate() {
        messages = ssrx::algo::demodulate(
            std::span<const Detection>(detections.data(), detections.size()),
            std::span<const value_type>(this->buffer(0), this->fft_size()),
            this->time(),
            nsamples_zero_padding(),
            nsamples_preamble,
            nsamples_data_long,
            ncoh,
            sampling_rate,
            modes);
    }
};

template<typename SDRImpl>
class SSRX {

    using value_type = typename SDRImpl::value_type;

    size_t nthreads;
    double snr_preamble_high;
    size_t ncoh;

    std::vector<value_type> preamble;

    std::vector<value_type> pp;

    ssrx::ModesDecoder modes;

    SDRImpl sdr_;

public:

    SSRX(sdr::Configuration&& conf)
        : nthreads(conf["correlator"]["nthreads"].as<size_t>())
        , snr_preamble_high(db_to_power_ratio(conf["detector"]["snr_preamble_high"].as<double>()))
        , ncoh(conf["detector"]["coherent_integration"].as<size_t>())
        , modes(ssrx::config::expand_path(conf["demodulator"]["database"].as<std::string>()))
        , sdr_(std::forward<sdr::Configuration>(conf))
    {
        sdr_.initialize_sdr();

        // ________________________________________________________________________
        // ````````````````````````````````````````````````````````````````````````
        //                          Ringbuffer Setup
        // ________________________________________________________________________
        // ````````````````````````````````````````````````````````````````````````

        // Calculate the number of samples of Mode-S preamble and data block.
        // https://www.radartutorial.eu/13.ssr/sr24.en.html
        // Size of Mode-S preamble in sampling rate (8 bits).
        auto modes_preamble_size = static_cast<size_t>(8e-6 * sdr_.sampling_rate());
        // Size of Mode-S data in long format (112 bits).
        auto modes_data_long_size = static_cast<size_t>(112e-6 * sdr_.sampling_rate());
        // Total size of Mode-S long message.
        auto modes_long_size = modes_preamble_size + modes_data_long_size;

        auto fft_size = sdr_.fft_size();

        std::cerr << std::endl;
        std::cerr << "Sizes:" << std::endl;
        std::cerr << "  Requested each ringbuffer duration: " << sdr_.ringbuffer_duration() << std::endl;
        std::cerr << "  Requested no. ringbuffer blocks: " << sdr_.ringbuffer_size() << std::endl;
        std::cerr << "  FFT size on ringbuffer: " << fft_size << std::endl;
        std::cerr << "  Mode-S preamble size: " << modes_preamble_size << std::endl;
        std::cerr << "  Mode-S long data size: " << modes_data_long_size << std::endl;
        std::cerr << "  Mode-S total long message size: " << modes_long_size << std::endl;
        std::cerr << std::endl;

        sdr_.initialize_ringbuffer(
            fft_size,
            modes_preamble_size,
            modes_data_long_size,
            ncoh,
            sdr_.sampling_rate(),
            modes
        );

        // ________________________________________________________________________
        // ````````````````````````````````````````````````````````````````````````
        //                           Detector Setup
        // ________________________________________________________________________
        // ````````````````````````````````````````````````````````````````````````

        preamble = ssrx::modes::make_modes_preamble(modes_preamble_size, sdr_.sampling_rate());
        
        // Visualization of Mode-S preamble.
        ssrx::modes::print_preamble(std::span<const value_type>(preamble.data(), preamble.size()), std::cerr);
        std::cerr << std::endl << std::endl;

        // FFT of the preamble.
        pp.resize(fft_size);
        ssrx::modes::compute_preamble_fft(
            std::span<const value_type>(preamble.data(), preamble.size()),
            std::span<value_type>(pp.data(), pp.size()));
    }

    time_t synchronize(bool no_pps = false) {
        return sdr_.synchronize(no_pps);
    }

    void start_sampling(time_t when = 0) {
        sdr_.start_sampling(when);
    }

    void start_monitor() {
        sdr_.start_monitor();
    }

    void start_amphist() {
        sdr_.start_amphist();
    }

    void start_integrity_tester() {
        sdr_.start_integrity_tester();
    }

    void request_stop() {
        sdr_.request_stop();
    }

    void stop() {
        sdr_.stop();
    }

    bool is_running() const {
        return sdr_.is_running();
    }

public:

    void start_correlator() {
        for (size_t i = 0; i < nthreads; ++i) {
            sdr_.start_thread([this, i](size_t) {

                auto sock_rx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["sampler"]["mq"], "Correlator", true, sdr_.socket_log_mutex());
                auto sock_tx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["correlator"]["mq"], i, "Correlator", false, sdr_.socket_log_mutex());

                while (sdr_.is_running()) {

                    zmq::message_t recv;
                    if (sdr_.is_running() && !sock_rx.recv(recv, zmq::recv_flags::dontwait))
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        continue;
                    }

                    auto iread = *reinterpret_cast<size_t*>(recv.data());

                    auto* input = &sdr_.ringbuffer()[iread].content();

                    input->correlate(pp);

                    if (sdr_.is_running() && !sock_tx.send(recv, zmq::send_flags::none)) {
                        std::cerr << "Error: zmq failed: " << sdr_.config()["correlator"]["mq"]["addr"] << std::endl;
                        sdr_.request_stop();
                        return;
                    }
                }
            }, i);
        }

    }

    /// @brief Detector thread.
    /// @param i 
    void start_detector() {
        for (size_t i = 0; i < nthreads; ++i) {
            sdr_.start_thread([this, i](size_t) {

                auto sock_rx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["correlator"]["mq"], i, "Detector", true, sdr_.socket_log_mutex());
                auto sock_tx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["detector"]["mq"], i, "Detector", false, sdr_.socket_log_mutex());

                while (sdr_.is_running()) {

                    zmq::message_t recv;
                    if (sdr_.is_running() && !sock_rx.recv(recv, zmq::recv_flags::dontwait))
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        continue;
                    }

                    auto iread = *reinterpret_cast<size_t*>(recv.data());
                    auto* input = &sdr_.ringbuffer()[iread].content();

                    input->detect_preamble(snr_preamble_high);

                    if (sdr_.is_running() && !sock_tx.send(recv, zmq::send_flags::none)) {
                        std::cerr << "Error: zmq failed: " << sdr_.config()["detector"]["mq"]["addr"] << std::endl;
                        sdr_.request_stop();
                        return;
                    }
                }
            }, i);
        }
    }

    void start_demodulator() {
        for (size_t i = 0; i < nthreads; ++i) {
            sdr_.start_thread([this, i](size_t) {

                auto sock_rx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["detector"]["mq"], i, "Demodulator", true, sdr_.socket_log_mutex());
                auto sock_tx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["demodulator"]["mq"], "Demodulator", false, sdr_.socket_log_mutex());

                while (sdr_.is_running()) {

                    zmq::message_t recv;
                    if (sdr_.is_running() && !sock_rx.recv(recv, zmq::recv_flags::dontwait))
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        continue;
                    }

                    auto iread = *reinterpret_cast<size_t*>(recv.data());
                    auto* input = &sdr_.ringbuffer()[iread];

                    input->content().demodulate();

                    if (sdr_.is_running() && !sock_tx.send(recv, zmq::send_flags::none)) {
                        std::cerr << "Error: zmq failed: " << sdr_.config()["demodulator"]["mq"]["addr"] << std::endl;
                        sdr_.request_stop();
                        return;
                    }
                }
            }, i);
        }
    }

    void start_collector() {
        sdr_.start_thread([this] {
            auto sock_rx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["demodulator"]["mq"], "Collector", true, sdr_.socket_log_mutex());
            auto sock_tx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["collector"]["mq"], "Data", false, sdr_.socket_log_mutex());
            auto waveform = sdr_.config()["collector"]["waveform"].template as<bool>(false);

            while (sdr_.is_running()) {

                zmq::message_t recv;
                if (sdr_.is_running() && !sock_rx.recv(recv, zmq::recv_flags::dontwait))
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                
                auto iread = *reinterpret_cast<size_t*>(recv.data());
                auto* input = &sdr_.ringbuffer()[iread].content();

                for (auto& msg : input->messages) {
                    size_t nc = msg.text.size();
                    if (!waveform) {
                        msg.samples.clear();
                    }
                    size_t payload_bytes = msg.samples.size() * sizeof(typename decltype(msg.samples)::value_type);
                    
                    Header header = ssrx::build_header(
                        msg.time,
                        msg.rssi,
                        static_cast<uint32_t>(nc),
                        static_cast<uint32_t>(ncoh),
                        static_cast<uint32_t>(msg.samples.size() / ncoh / 2),
                        VTYPE_C32);

                    zmq::message_t hdr(sizeof(Header));
                    std::memcpy(hdr.data(), &header, sizeof(Header));  // TODO: move header into rb_data to avoid copy
                    zmq::message_t message(msg.text.data(), msg.text.size());
                    zmq::message_t packed(msg.samples.data(), payload_bytes);

                    if (
                        sdr_.is_running() &&
                        !(sock_tx.send(hdr, zmq::send_flags::sndmore) &&
                          sock_tx.send(message, zmq::send_flags::sndmore) &&
                          sock_tx.send(packed, zmq::send_flags::none))
                    ) {
                        std::cerr << "Error: zmq failed: " << "sock_tx" << std::endl;
                        sdr_.request_stop();
                        return;
                    }
                }

                // Finish this buffer.
                sdr_.ringbuffer()[iread].set_state_writable();
            }
        });
    }

    void start_printer() {
        auto enable = sdr_.config()["printer"]["enable"].template as<bool>(true);
        if (!enable) return;  // Do not start printer.

        auto show_datetime = sdr_.config()["printer"]["columns"]["datetime"].template as<bool>(false);
        auto show_rssi = sdr_.config()["printer"]["columns"]["rssi"].template as<bool>(false);
        auto show_raw = sdr_.config()["printer"]["columns"]["raw"].template as<bool>(false);
        auto delimiter = sdr_.config()["printer"]["delimiter"].template as<std::string>();
        auto uppercase = sdr_.config()["printer"]["uppercase"].template as<bool>(false);

        sdr_.start_thread([this, show_datetime, show_rssi, show_raw, delimiter, uppercase] {
            size_t total_count = 0;
            auto sock_rx = ssrx::mq::create_socket(sdr_.zmq_context(), sdr_.config()["collector"]["mq"], "", true, sdr_.socket_log_mutex());
            sock_rx.set(zmq::sockopt::rcvtimeo, 10);
            while (sdr_.is_running()) {
                std::vector<zmq::message_t> parts;
                if (!zmq::recv_multipart(sock_rx, std::back_inserter(parts)))
                {
                    continue;
                }

                Header* header = reinterpret_cast<Header*>(parts[0].data());

                std::string message(static_cast<const char*>(parts[1].data()), parts[1].size());
                time_t seconds = static_cast<time_t>(header->seconds);
                std::tm local_tm = *std::gmtime(&seconds);

                bool shown = false;
                if (show_datetime) {
                    std::cout << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S") << "."
                              << std::fixed << std::setprecision(0) << std::setw(6) << std::setfill('0') << (header->femtoseconds / 1e9);
                    shown = true;
                }
                if (show_rssi) {
                    if (shown) {
                        std::cout << delimiter;
                    }
                    std::cout << std::setprecision(2) << std::setw(5) << header->rssi << " dB";
                    shown = true;
                }
                if (show_raw) {
                    if (shown) {
                        std::cout << delimiter;
                    }
                    std::cout << std::setw(28) << std::setfill(' ') << std::left << ssrx::bin2hex(reinterpret_cast<const uint8_t*>(message.data()), message.size(), uppercase);
                    shown = true;
                }
                if (shown) {
                    std::cout << std::endl;
                }
                ++total_count;
            }
            std::cerr << "No. Total Decoded Messages: " << total_count << std::endl;
        });
    }
};

}

#endif

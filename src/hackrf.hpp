#ifndef SSRX_HACKRF_HPP
#define SSRX_HACKRF_HPP

#include <libhackrf/hackrf.h>
#include "sdr.hpp"


#define TRY_READ(expr) [&] { try { return expr; } catch (const std::exception& e) { std::cerr << "Error: in expression " << #expr << ": " << e.what() << std::endl; throw; } }()


namespace ssrx { namespace sdr {

template<typename rb_data>
class HackRF : public SDR<rb_data> {
public:

    using value_type = typename rb_data::value_type;
    using Base = SDR<rb_data>;

public:

    HackRF(
        Configuration&& conf
    )
        : SDR<rb_data>(std::forward<Configuration>(conf))
        , hackrf_(nullptr)
    { }

    HackRF(
        const Configuration& conf
    )
        : SDR<rb_data>(conf)
        , hackrf_(nullptr)
    { }

    virtual ~HackRF() { }

    virtual void stop() override {
        int rc;
        this->request_stop();
        if (hackrf_ && hackrf_is_streaming(hackrf_) == HACKRF_TRUE) {
            check(hackrf_stop_rx(hackrf_));
        }
        if (hackrf_) {
            rc = hackrf_close(hackrf_);
            if (rc != HACKRF_SUCCESS) {
                std::cerr << hackrf_error_name((hackrf_error)rc) << std::endl;
            }
            hackrf_ = nullptr;
        }
        rc = hackrf_exit();
        if (rc != HACKRF_SUCCESS) {
            std::cerr << hackrf_error_name((hackrf_error)rc) << std::endl;
        }
        this->Base::stop();
    }

    void check(int rc) {
        if (rc != HACKRF_SUCCESS) {
            throw std::runtime_error(hackrf_error_name((hackrf_error)rc));
        }
    }

    virtual void initialize_sdr() override {
        auto hackrf_conf = this->conf["hackrf"];
        if (!hackrf_conf || !hackrf_conf.IsMap()) {
            throw std::runtime_error("missing required 'hackrf' section in configuration");
        }

        auto device_serial = hackrf_conf["device"]["serial"].template as<std::string>("");

        // Rx gain control.
        auto lna_gain = TRY_READ(hackrf_conf["lna_gain"].template as<uint32_t>());
        auto vga_gain = TRY_READ(hackrf_conf["vga_gain"].template as<uint32_t>());

        // Rx sampling rate.
        auto rate = TRY_READ(::ssrx::config::read_frequency_hz(hackrf_conf["sampling_rate"]));

        // Rx center frequency.
        auto freq = static_cast<uint64_t>(
            TRY_READ(::ssrx::config::read_frequency_hz(hackrf_conf["center_frequency"])));

        // Rx filter bandwidth.
        auto bandwidth = static_cast<uint32_t>(
            TRY_READ(::ssrx::config::read_frequency_hz(hackrf_conf["band_width"])));

        // set_amp
        auto set_amp = TRY_READ(hackrf_conf["enable_amp"].template as<bool>());

        auto rc = hackrf_init();
        if (rc != HACKRF_SUCCESS) {
            throw std::runtime_error(hackrf_error_name((hackrf_error)rc));
        }

        if (!device_serial.empty()) {
            check(hackrf_open_by_serial(device_serial.c_str(), &hackrf_));
        } else {
            check(hackrf_open(&hackrf_));
        }

        // Set Rx center frequency.
        std::cerr
            << "Setting Rx Frequency: " << std::fixed << std::setprecision(3) << (freq / 1e6) << " MHz (" << freq << " Hz)"
            << std::endl;
        check(hackrf_set_freq(hackrf_, freq));

        // Set Rx sampling rate.
        std::cerr
            << "Setting Rx Sampling Rate: " << std::fixed << std::setprecision(3) << (rate / 1e6) << " MSPS"
            << std::endl;
        check(hackrf_set_sample_rate(hackrf_, rate));
        this->rate_ = rate;

        if (set_amp) {
            std::cerr
                << "Enabling RF amplifier (+ ~11 dB)"
                << std::endl;
            check(hackrf_set_amp_enable(hackrf_, 1));
        }

        // Set Rx filter bandwidth.
        uint32_t bw_actual = hackrf_compute_baseband_filter_bw(bandwidth);
        std::cerr
            << "Setting Rx Bandwidth: " << std::fixed << std::setprecision(3) << (bw_actual / 1e6) << " MHz (" << bw_actual << " Hz)"
            << std::endl;
        check(hackrf_set_baseband_filter_bandwidth(hackrf_, bw_actual));

        std::cerr
            << "Setting Rx Gain: LNA = " << lna_gain << " dB, VGA = " << vga_gain << " dB"
            << std::endl;
        check(hackrf_set_lna_gain(hackrf_, lna_gain));
        check(hackrf_set_vga_gain(hackrf_, vga_gain));
    }

    /// @brief Synchronize time source with external PPS.
    ///        Absolute time refers to PC's clock.
    /// @param no_pps If set to true, computer clock is used and don't wait for the PPS.
    /// @return Return time at synchronization in time_t.
    time_t synchronize(bool no_pps = false) override {
        if (!no_pps) {
            throw std::runtime_error("PPS Synchronization is currently not available.");
        }

        // Synchronize the next PPS with the PC's clock.
        // Assuming this computer has fairly accurate clock to at least several milliseconds...
        auto now = std::chrono::system_clock::now();
        auto t0 = std::chrono::round<std::chrono::seconds>(now).time_since_epoch().count();
        // Time at synchronization in time_t compatible int64_t.
        auto t1 = static_cast<time_t>(t0 + 1);
        if (no_pps) {
            
        }
        auto utc = std::gmtime(&t1);
        std::cerr
            << "Time synchronized: " << t1
            << " (" << std::put_time(utc, "%Y/%m/%d %H:%M:%S UTC") << ")" << std::endl << std::endl;
        return t1;
    }

    static int rx_callback(hackrf_transfer* transfer) {

        auto size = transfer->valid_length / 2;
        auto beg = reinterpret_cast<std::complex<int8_t>*>(transfer->buffer);
        // this HackRF object.
        auto* obj = reinterpret_cast<HackRF*>(transfer->rx_ctx);
        if (!obj || !obj->is_running()) {
            return HACKRF_ERROR_OTHER;
        }

        // // Start time of each packet.
        auto* packet = &obj->rb.write_buffer().content();
        
        // // Store the buffer start time and data.
        packet = obj->store(
            obj->sample_time_,
            beg, size);
        
        // // Advance clock.
        obj->sample_time_ += size / obj->rate_;
        
        if (!packet) {
            return HACKRF_ERROR_OTHER;
        }
        if (!obj->is_running()) {
            return HACKRF_ERROR_OTHER;
        }

        return HACKRF_SUCCESS;
    }

    /// @brief Start sampling. Currently the sample start time is very coarse, as it is taken from the PC's clock.
    /// @param when Ignored.
    void start_sampling(time_t when = 0) override {
        if (!when) {
            when = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        this->Base::start_sampling(when);
        // this->wait_start_time(when);
        sample_time_ = TimeSpec(static_cast<int64_t>(when));
        sample_start_time_ = when;

        check(hackrf_start_rx(hackrf_, &rx_callback, this));

        std::cerr << std::endl;
        std::cerr << "-- SAMPLING STARTED --" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Press Ctrl + C to exit." << std::endl;
        std::cerr << std::endl;
    }

public:

private:
    hackrf_device* hackrf_;

    time_t sample_start_time_;
    TimeSpec sample_time_;

    zmq::socket_t socket;

};

} }

#endif

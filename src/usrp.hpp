#ifndef SSRX_USRP_HPP
#define SSRX_USRP_HPP

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/tune_request.hpp>
#include <thread>
#include <vector>
#include <chrono>
#include <complex>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ctime>
#include <type_traits>
#include "sdr.hpp"

#define TRY_READ(expr) [&] { try { return expr; } catch (const std::exception& e) { std::cerr << "Error: in expression " << #expr << ": " << e.what() << std::endl; throw; } }()

namespace ssrx { namespace sdr {

/// @brief TimeSpec <-> uhd::time_spec_t conversion helpers
namespace usrp_helper {
    /// @brief Convert uhd::time_spec_t to TimeSpec
    inline TimeSpec to_timespec(const uhd::time_spec_t& uhd_time) {
        return TimeSpec(
            uhd_time.get_full_secs(),
            uhd_time.get_frac_secs()
        );
    }

    /// @brief Convert TimeSpec to uhd::time_spec_t
    inline uhd::time_spec_t from_timespec(const TimeSpec& ts) {
        return uhd::time_spec_t(
            static_cast<double>(ts.get_full_secs()) + ts.get_frac_secs()
        );
    }

    /// @brief Convert Unix time seconds to uhd::time_spec_t
    inline uhd::time_spec_t from_unix_seconds(time_t seconds) {
        return uhd::time_spec_t(static_cast<int64_t>(seconds), 0.0);
    }
}

template<typename rb_data>
class USRP : public SDR<rb_data> {
public:

    using value_type = typename rb_data::value_type;
    using Base = SDR<rb_data>;

public:

    USRP(
        Configuration&& conf
    )
        : SDR<rb_data>(std::forward<Configuration>(conf))
        , usrp_(nullptr)
        , rx_stream_(nullptr)
    // Removed socket initialization to avoid nullptr error
    { }

    USRP(
        const Configuration& conf
    )
        : SDR<rb_data>(conf)
        , usrp_(nullptr)
        , rx_stream_(nullptr)
    { }

    virtual ~USRP() { }

    virtual void initialize_sdr() override {
        // Extract USRP configuration from YAML
        auto usrp_conf = this->conf["usrp"];
        if (!usrp_conf || !usrp_conf.IsMap()) {
            throw std::runtime_error("missing required 'usrp' section in configuration");
        }

        auto args = usrp_conf["args"].template as<std::string>("");
        auto mboard = usrp_conf["mboard"].template as<size_t>(0);
        auto time_source = usrp_conf["time"]["source"].template as<std::string>("");
        auto clock_source = usrp_conf["clock"]["source"].template as<std::string>("");

        auto subdev = usrp_conf["subdev"].template as<std::string>("");
        auto antenna = usrp_conf["antenna"].template as<std::string>("");

        auto iq_balance = TRY_READ(usrp_conf["iq_balance"].template as<bool>());
        auto gain = TRY_READ(usrp_conf["gain"].template as<double>());

        // Rx sampling rate
        auto rate = TRY_READ(::ssrx::config::read_frequency_hz(usrp_conf["sampling_rate"]));

        // Rx center frequency
        auto freq = TRY_READ(::ssrx::config::read_frequency_hz(usrp_conf["center_frequency"]));

        // Rx filter bandwidth
        auto bandwidth = TRY_READ(::ssrx::config::read_frequency_hz(usrp_conf["band_width"]));

        // Data format
        auto format_wire = TRY_READ(usrp_conf["data_format"]["wire"].template as<std::string>());
        auto format_cpu = TRY_READ(usrp_conf["data_format"]["cpu"].template as<std::string>());
        if (format_cpu != "fc32" && format_cpu != "fc64") {
            throw std::runtime_error(
                std::string("Error: CPU data format must be fc32 or fc64, but config specifies: ") + format_cpu
            );
        }

        using raw_scalar_t = typename Base::raw_t::value_type;
        std::string expected_format_cpu;
        if constexpr (std::is_same_v<raw_scalar_t, float>) {
            expected_format_cpu = "fc32";
        } else if constexpr (std::is_same_v<raw_scalar_t, double>) {
            expected_format_cpu = "fc64";
        } else {
            throw std::runtime_error(
                "Error: USRP template raw scalar type must be float or double"
            );
        }

        if (format_cpu != expected_format_cpu) {
            throw std::runtime_error(
                std::string("Error: CPU data format mismatch. config=") + format_cpu
                + ", template expects " + expected_format_cpu
            );
        }

        // Create and configure USRP
        usrp_ = uhd::usrp::multi_usrp::make(args);

        std::cerr << std::endl;

        // Set subdevice
        if (!subdev.empty()) {
            usrp_->set_rx_subdev_spec(subdev);
        }

        // Set antenna
        if (!antenna.empty()) {
            usrp_->set_rx_antenna(antenna);
        }

        // Set time source
        if (!time_source.empty()) {
            usrp_->set_time_source(time_source, mboard);
        }
        std::cerr
            << "Time source: " << usrp_->get_time_source(mboard) << std::endl << std::endl;

        // Set clock source
        if (!clock_source.empty()) {
            usrp_->set_clock_source(clock_source, mboard);
        }
        std::cerr
            << "Clock source: " << usrp_->get_clock_source(mboard) << std::endl << std::endl;

        // IQ balancing
        usrp_->set_rx_iq_balance(iq_balance);
        std::cerr
            << "Rx IQ Balancing: " << std::boolalpha << iq_balance << std::endl << std::endl;

        // Set Rx gain
        std::cerr
            << "Setting Rx Gain: " << gain << " dB ..."
            << std::endl;
        usrp_->set_rx_gain(gain);
        std::cerr
            << "  Actual Rx Gain: " << std::fixed << std::setprecision(3) << usrp_->get_rx_gain() << " dB"
            << std::endl
            << std::endl;

        // Set Rx sampling rate
        std::cerr
            << "Setting Rx Sampling Rate: " << std::fixed << std::setprecision(3) << (rate / 1e6) << " MSPS ..."
            << std::endl;
        usrp_->set_rx_rate(rate);
        auto rate_actual = usrp_->get_rx_rate();
        std::cerr
            << "  Actual Rx Sampling Rate: " << std::fixed << std::setprecision(12) << rate_actual << " SPS"
            << std::endl
            << std::endl;
        this->rate_ = rate_actual;

        // Set Rx center frequency
        std::cerr
            << "Setting Rx Frequency: " << std::fixed << std::setprecision(3) << (freq / 1e6) << " MHz ..."
            << std::endl;
        usrp_->set_rx_freq(freq);
        std::cerr
            << "  Actual Rx Frequency: " << std::fixed << std::setprecision(12) << usrp_->get_rx_freq() << " Hz"
            << std::endl
            << std::endl;

        // Set Rx filter bandwidth
        std::cerr
            << "Setting Rx Bandwidth: " << std::fixed << std::setprecision(3) << (bandwidth / 1e6) << " MHz ..."
            << std::endl;
        usrp_->set_rx_bandwidth(bandwidth);
        std::cerr
            << "  Actual Rx Bandwidth: " << std::fixed << std::setprecision(12) << usrp_->get_rx_bandwidth() << " Hz"
            << std::endl
            << std::endl;

        // Setup streaming
        uhd::stream_args_t stream_args(format_cpu, format_wire);
        stream_args.channels = {0};
        rx_stream_ = usrp_->get_rx_stream(stream_args);
    }

    virtual time_t synchronize(bool no_pps = false) override {
        if (!no_pps) {
            // Wait for the next PPS
            std::cerr << "Waiting for the next PPS ..." << std::endl;
            const uhd::time_spec_t time_last_pps = usrp_->get_time_last_pps();
            while (time_last_pps == usrp_->get_time_last_pps()) {
                if (!this->running) {
                    std::cerr << std::endl << "-- CANCELED --" << std::endl;
                    throw std::runtime_error("PPS synchronization canceled");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // Synchronize with PC clock.
        // Keep this after PPS wait to match legacy behavior.
        auto now = std::chrono::system_clock::now();
        auto t0 = std::chrono::round<std::chrono::seconds>(now).time_since_epoch().count();
        auto t1 = static_cast<time_t>(t0 + 1);

        if (!no_pps) {
            // Set time at next PPS
            usrp_->set_time_next_pps(usrp_helper::from_unix_seconds(t1));
            std::this_thread::sleep_for(std::chrono::seconds(1));  // Wait for PPS to latch
            std::cerr << "  ";
        } else {
            // Set time immediately using PC clock
            usrp_->set_time_now(usrp_helper::from_unix_seconds(t1));
        }

        auto utc = std::gmtime(&t1);
        std::cerr
            << "Time synchronized: " << t1
            << " (" << std::put_time(utc, "%Y/%m/%d %H:%M:%S UTC") << ")" << std::endl << std::endl;

        return t1;
    }

    virtual void start_sampling(time_t when = 0) override {
        if (!when) {
            when = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        }

        this->Base::start_sampling(when);
        auto after = 1;
        auto sample_start_at = when + after;

        sample_time_ = TimeSpec(static_cast<int64_t>(sample_start_at));
        sample_start_time_ = when;

        // Issue stream command
        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        stream_cmd.stream_now = false;
        stream_cmd.time_spec = usrp_helper::from_unix_seconds(sample_start_at);
        rx_stream_->issue_stream_cmd(stream_cmd);

        // Wait until just before the requested start time.
        this->wait_start_time(sample_start_at);

        this->start_thread([this](size_t) {
            double timeout = 1.0;

            // Buffer for recv (scalar type validated against config at initialization).
            std::vector<void*> buffs(rx_stream_->get_num_channels());
            std::vector<typename Base::raw_t> buff(rx_stream_->get_max_num_samps());
            buffs[0] = buff.data();

            // Convert to the raw type used by RingBufferData.
            std::vector<typename Base::raw_t> raw(buff.size());

            // Metadata filled by recv.
            uhd::rx_metadata_t md;

            while (this->is_running()) {
                size_t num_rx_samps = rx_stream_->recv(buffs, buff.size(), md, timeout, true);

                if (!this->is_running()) {
                    break;
                }

                // Handle the error code exactly like legacy behavior.
                if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                    std::cerr << "Error: Timeout" << std::endl;
                    this->request_stop();
                    break;
                }
                if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                    auto* packet = &this->ringbuffer().write_buffer().content();
                    auto nsamples_lost = packet->size();
                    std::cerr
                        << "Error: " << md.strerror() << ": " << std::dec
                        << nsamples_lost << " samples were discarded" << std::endl;
                    // Reset current write buffer to match legacy overflow handling.
                    packet->reset();
                    continue;
                }
                if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                    std::cerr << "Error: " << md.strerror() << std::endl;
                    this->request_stop();
                    break;
                }

                if (num_rx_samps == 0) {
                    continue;
                }

                using raw_scalar_t = typename Base::raw_t::value_type;
                if constexpr (std::is_integral_v<raw_scalar_t>) {
                    auto clip_to_raw = [](double x) -> raw_scalar_t {
                        auto y = std::clamp(x, -1.0, 1.0) * static_cast<double>(std::numeric_limits<raw_scalar_t>::max());
                        return static_cast<raw_scalar_t>(std::lrint(y));
                    };

                    std::transform(
                        buff.begin(),
                        buff.begin() + num_rx_samps,
                        raw.begin(),
                        [clip_to_raw](const typename Base::raw_t& v) {
                            return typename Base::raw_t(clip_to_raw(v.real()), clip_to_raw(v.imag()));
                        }
                    );
                } else {
                    std::transform(
                        buff.begin(),
                        buff.begin() + num_rx_samps,
                        raw.begin(),
                        [](const typename Base::raw_t& v) {
                            return typename Base::raw_t(v.real(), v.imag());
                        }
                    );
                }

                auto ts = sample_time_;
                if (md.has_time_spec) {
                    ts = usrp_helper::to_timespec(md.time_spec);
                }

                if (!this->store(ts, raw.data(), num_rx_samps)) {
                    this->request_stop();
                    break;
                }

                sample_time_ = ts + num_rx_samps / this->rate_;
            }
        }, 0);

        std::cerr << std::endl;
        std::cerr << "-- SAMPLING STARTED --" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Press Ctrl + C to exit." << std::endl;
        std::cerr << std::endl;
    }

    virtual void stop() override {
        this->request_stop();
        if (rx_stream_) {
            rx_stream_->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        }
        this->Base::stop();
    }

private:
    /// UHD multi-USRP device
    uhd::usrp::multi_usrp::sptr usrp_;

    /// RX stream
    uhd::rx_streamer::sptr rx_stream_;

    /// Sample time for packet tracking
    TimeSpec sample_time_;

    /// Sample start time (epoch seconds)
    time_t sample_start_time_;

};

} }

#endif

#ifndef SSRX_SDR_HPP
#define SSRX_SDR_HPP

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <limits>
#include <complex>
#include <thread>
#include <vector>
#include <memory>
#include <utility>
#include <mutex>
#include <ranges>
#include <type_traits>
#include <fftw3.h>
#include <yaml-cpp/yaml.h>
#include "ringbuffer.hpp"
#include "confighelper.hpp"
#include "mqhelper.hpp"
#include "amphist.hpp"
#include "monitor_bits.hpp"

namespace ssrx { namespace sdr {

template<typename scalar_t>
struct FFTWBridge;

template<>
struct FFTWBridge<float> {
    using plan_type = fftwf_plan_s;

    static void* malloc(size_t size) {
        return fftwf_malloc(size);
    }

    static void free(void* ptr) {
        fftwf_free(ptr);
    }

    static plan_type* plan_dft_1d(int n, std::complex<float>* in, std::complex<float>* out, int sign, unsigned flags) {
        return fftwf_plan_dft_1d(
            n,
            reinterpret_cast<fftwf_complex*>(in),
            reinterpret_cast<fftwf_complex*>(out),
            sign,
            flags);
    }

    static void execute(plan_type* plan) {
        fftwf_execute(reinterpret_cast<fftwf_plan>(plan));
    }

    static void destroy_plan(plan_type* plan) {
        fftwf_destroy_plan(reinterpret_cast<fftwf_plan>(plan));
    }
};

template<>
struct FFTWBridge<double> {
    using plan_type = fftw_plan_s;

    static void* malloc(size_t size) {
        return fftw_malloc(size);
    }

    static void free(void* ptr) {
        fftw_free(ptr);
    }

    static plan_type* plan_dft_1d(int n, std::complex<double>* in, std::complex<double>* out, int sign, unsigned flags) {
        return fftw_plan_dft_1d(
            n,
            reinterpret_cast<fftw_complex*>(in),
            reinterpret_cast<fftw_complex*>(out),
            sign,
            flags);
    }

    static void execute(plan_type* plan) {
        fftw_execute(reinterpret_cast<fftw_plan>(plan));
    }

    static void destroy_plan(plan_type* plan) {
        fftw_destroy_plan(reinterpret_cast<fftw_plan>(plan));
    }
};

/// @brief Femto seconds.
typedef std::chrono::duration<int64_t, std::femto> femtoseconds;

/// @brief uhd::time_spec_t like time representation.
///        Pair of `(seconds, femtoseconds)`.
struct TimeSpec {
    std::chrono::seconds seconds;
    sdr::femtoseconds femtoseconds;

    inline TimeSpec()
        : seconds(0)
        , femtoseconds(0)
    { }

    inline TimeSpec(std::chrono::seconds seconds, sdr::femtoseconds femtoseconds)
        : seconds(seconds)
        , femtoseconds(femtoseconds)
    {
        init();
    }

    inline TimeSpec(int64_t full_secs, double frac_secs = 0.)
        : seconds(full_secs)
        , femtoseconds()
    {
        int64_t full, frac;
        std::tie(full, frac) = split(frac_secs);
        seconds += std::chrono::seconds(full);
        femtoseconds = sdr::femtoseconds(frac);
        init();
    }

    inline TimeSpec(double frac_secs)
        : TimeSpec(0, frac_secs)
    { }

    inline void init() {
        while (femtoseconds < femtoseconds::zero()) {
            seconds -= std::chrono::seconds(1);
            femtoseconds += std::chrono::seconds(1);
        }
        while (femtoseconds > std::chrono::seconds(1)) {
            seconds += std::chrono::seconds(1);
            femtoseconds -= std::chrono::seconds(1);
        }
    }

    template<typename T>
    static std::pair<int64_t, int64_t> split(const T& seconds) {
        auto full = static_cast<int64_t>(seconds);
        auto frac = static_cast<int64_t>((seconds - full) * std::femto::den);
        return std::make_pair(full, frac);
    }

    template<typename T>
    TimeSpec& operator +=(const T& value) {
        int64_t full, frac;
        std::tie(full, frac) = split(value);
        seconds += std::chrono::seconds(full);
        femtoseconds += sdr::femtoseconds(frac);
        init();
        return *this;
    }

    template<typename T>
    TimeSpec operator +(const T& seconds) const {
        auto copy = *this;
        copy += seconds;
        return copy;
    }

    inline TimeSpec operator +(const TimeSpec& other) const {
        auto full = seconds + other.seconds;
        auto frac = femtoseconds + other.femtoseconds;
        return TimeSpec(full, frac);
    }
    
    inline TimeSpec operator -(const TimeSpec& other) const {
        auto full = seconds - other.seconds;
        auto frac = femtoseconds - other.femtoseconds;
        return TimeSpec(full, frac);
    }

    operator double() const {
        return seconds.count() + femtoseconds.count() / 1e15;
    }

    inline int64_t get_full_secs() const {
        return seconds.count();
    }

    inline double get_frac_secs() const {
        return femtoseconds.count() / 1e15;
    }
};

template<typename raw_t, typename complex_t>
class RingBufferData {
public:
    using raw_type = raw_t;
    using value_type = complex_t;
    using scalar_type = typename value_type::value_type;
    using fftw_bridge = FFTWBridge<scalar_type>;
    using fftw_plan_type = typename fftw_bridge::plan_type;

private:

    struct fftw_free_deleter;
    struct fftw_destroy_plan_deleter;

    /// @brief Current size.
    size_t curr_size_;

    /// @brief The number of raw buffer.
    size_t nraw_;

    /// @brief Zero padding at the beginning of the raw sample buffer.
    size_t nzeros_head_;

    /// @brief Zero padding at the end of the raw sample buffer.
    size_t nzeros_tail_;

    /// @brief The number of overlapped samples copied into the begining of the next buffer.
    size_t noverlapped_;

    /// @brief FFT size.
    size_t nfft_;

    /// @brief Raw sample buffer of the size `nraw_`.
    std::vector<raw_t> samples_;

    /// @brief FFT buffers each with the size `nfft_`.
    std::vector<std::unique_ptr<complex_t, fftw_free_deleter> > buffers_;

    /// @brief FFT plans.
    std::vector<std::unique_ptr<fftw_plan_type, fftw_destroy_plan_deleter> > plans_;

    /// @brief List of start time for each Rx packet.
    std::vector<TimeSpec> start_times;

public:

    /// @brief A sample buffer is as below:
    ///
    /// | <-------------------------- fft_size -------------------------> |  
    /// |                                                                 |
    /// |        | <------------------- size ------------------> |        |
    /// +-----------------------------------------------------------------+
    /// | nzeros | noverlapped     | Fresh samples | noverlapped | nzeros |
    /// | _head  | (from previous) |               | (to next)   |_tail   |
    /// +-----------------------------------------------------------------+
    /// |        |                                 |             |
    /// |        |        begin_overlapped()-------+             |
    /// |        |                     end_overlapped() ---------+
    /// |        +---- begin()                ^
    /// |        +---- time()                 |
    /// |                                     +--- end()
    /// +---- samples()                
    ///
    /// @param raw_size 
    /// @param nzeros_head 
    /// @param nzeros_tail 
    /// @param noverlapped 
    /// @param fft_size 
    /// @param nbuffers 
    RingBufferData(
        size_t raw_size,
        size_t nzeros_head,
        size_t nzeros_tail,
        size_t noverlapped,
        size_t fft_size,
        size_t nbuffers
    )
        : curr_size_(0)
        , nraw_(raw_size)
        , nzeros_head_(nzeros_head)
        , nzeros_tail_(nzeros_tail)
        , noverlapped_(noverlapped)
        , nfft_(fft_size)
        , samples_(raw_size)
        , buffers_(nbuffers)
        , plans_()
        , start_times()
    {
        // samples_.reset(reinterpret_cast<complex_t*>(fftw_malloc(raw_size * sizeof(fftw_complex))));
        std::fill_n(samples_.begin(), nzeros_head_, raw_t());
        std::fill_n(samples_.begin() + nraw_ - nzeros_tail_, nzeros_tail_, raw_t());
        for (size_t i = 0; i < nbuffers; ++i) {
            buffers_[i].reset(reinterpret_cast<complex_t*>(fftw_bridge::malloc(fft_size * sizeof(complex_t))));
            std::fill_n(buffers_[i].get(), fft_size, complex_t());
        }
        reset();
    }

    RingBufferData(RingBufferData&&) = default;

    RingBufferData(const RingBufferData&) = delete;

    virtual ~RingBufferData() { }

    /// @brief Convert data from samples_ into buffers_[0].
    virtual void convert() {
        using input_type = typename raw_t::value_type;
        using output_type = typename complex_t::value_type;
        if constexpr (std::is_integral_v<input_type>) {
            auto den = static_cast<output_type>(std::numeric_limits<input_type>::max()) + 1;
            std::transform(
                this->begin(), this->end(),
                this->begin(0),
                [den](const auto& c) {
                    return value_type(c.real() / den, c.imag() / den);
                }
            );
        } else {
            std::transform(
                this->begin(), this->end(),
                this->begin(0),
                [](const auto& c) {
                    return value_type(
                        static_cast<output_type>(c.real()),
                        static_cast<output_type>(c.imag()));
                }
            );
        }
    }

    /// @brief Plan FFT from `buffers_[i]` to `buffers_[j]`.
    /// @param i Input index in `buffers_`.
    /// @param j Output index in `buffers_`.
    /// @param sign `FFTW_FORWARD` or `FFTW_BACKWORD`
    size_t plan(size_t i, size_t j, int sign) {
        size_t index = plans_.size();
        // std::cerr << "Plan " << index << ": " << i << " -> " << j  << ", " << nfft_ << std::endl;
        plans_.emplace_back(
            std::unique_ptr<fftw_plan_type, fftw_destroy_plan_deleter>(fftw_bridge::plan_dft_1d(
                static_cast<int>(nfft_),
                buffers_[i].get(),
                buffers_[j].get(),
                sign,
                FFTW_MEASURE | FFTW_PRESERVE_INPUT)));
        return index;
    }

    /// @brief Execute FFT.
    /// @param i Plan index returned by `plan()`.
    void execute(size_t i) {
        fftw_bridge::execute(plans_[i].get());
    }

    raw_t* reset() {
        curr_size_ = nzeros_head_;
        start_times.clear();
        return end();
    }

    /// @brief Available space for writing.
    size_t available() const {
        return nraw_ - curr_size_ - nzeros_tail_;
    }

    size_t noverlapped() const {
        return noverlapped_;
    }

    /// @brief Copy the content from the iterator into the buffer.
    /// @tparam Iter 
    /// @param beg 
    /// @param prepared Prepared samples from SDR.
    /// @param written Written size returned to the caller.
    /// @return `beg + written`. Could be `beg + prepared` if buffer had enough space.
    template<typename Iter>
    Iter copy_from(Iter beg, size_t prepared, size_t& written) {
        written = std::min(prepared, available());
        std::copy_n(beg, written, end());
        curr_size_ += written;
        return beg + written;
    }

    void add_time(const TimeSpec& t) {
        start_times.push_back(t);
    }

    const raw_t* begin() const {
        return samples_.data() + nzeros_head_;
    }

    raw_t* begin() {
        return samples_.data() + nzeros_head_;
    }

    /// @brief Buffer begin()
    /// @param i 
    /// @return 
    const complex_t* begin(size_t i) const {
        return buffers_[i].get() + nzeros_head_;
    }

    /// @brief Buffer begin()
    /// @param i 
    /// @return 
    complex_t* begin(size_t i) {
        return buffers_[i].get() + nzeros_head_;
    }

    const raw_t* end() const {
        return samples_.data() + curr_size_;
    }

    raw_t* end() {
        return samples_.data() + curr_size_;
    }

    const raw_t* begin_overlapped() const {
        return samples_.data() + nfft_ - nzeros_tail_ - noverlapped_;
    }

    raw_t* begin_overlapped() {
        return samples_.data() + nfft_ - nzeros_tail_ - noverlapped_;
    }

    const raw_t* end_overlapped() const {
        return begin_overlapped() + noverlapped();
    }

    raw_t* end_overlapped() {
        return begin_overlapped() + noverlapped();
    }

    const raw_t* samples() const {
        return samples_.data();
    }

    raw_t* samples() {
        return samples_.data();
    }

    const complex_t* buffer(size_t i) const {
        return buffers_[i].get();
    }

    complex_t* buffer(size_t i) {
        return buffers_[i].get();
    }

    size_t fft_size() const {
        return nfft_;
    }

    /// @brief Valid number of samples.
    ///        
    size_t size() const {
        return curr_size_ - nzeros_head_;
    }

    size_t fresh_size() const {
        return size() - noverlapped();
    }

    TimeSpec time() const {
        return start_times[0];
    }

    /// @brief Time at the beggining of the overlapped region.
    /// @param sampling_rate Actual sampling rate.
    TimeSpec time_at_overlap(double sampling_rate) const {
        return time() + (size() - noverlapped()) / sampling_rate;
    }

private:
    
    struct fftw_free_deleter {
        void operator ()(void* m) const {
            fftw_bridge::free(m);
        }
    };

    struct fftw_destroy_plan_deleter {
        void operator ()(void* p) const {
            fftw_bridge::destroy_plan(reinterpret_cast<fftw_plan_type*>(p));
        }
    };
};

using Configuration = ::ssrx::config::Configuration;



template<typename rb_data>
class SDR {

public:

    using raw_t = typename rb_data::raw_type;
    using complex_t = typename rb_data::value_type;

public:

    /// @brief Initializes the SDR interface.
    /// @param path Path to a YAML configuration file.
    SDR(Configuration&& conf)
        : conf(std::forward<Configuration>(conf))
        , running(true)
        , rb()
    { }

    SDR(const Configuration& conf)
        : conf(conf)
        , running(true)
        , rb()
    { }

    virtual ~SDR() { }

    /// @brief Return true if threads are in running state.
    bool is_running() const {
        return running.load();
    }

    void request_stop() {
        running.store(false);
    }

    /// @brief Initialize the SDR.
    /// @note Must be overriden by the child class.
    virtual void initialize_sdr() { }

    /// @brief Initialize the ringbuffer.
    /// @tparam Args Types of parameters for initializing `rb_data`.
    /// @param args Parameters for initializing `rb_data`.
    template<typename... Args>
    void initialize_ringbuffer(Args&&... args) {
        auto rbnum = this->ringbuffer_size();
        this->rb.reset(rbnum, std::forward<Args>(args)...);
    }

    /// @brief Stop sampling and all other threads.
    virtual void stop() {
        request_stop();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        std::cerr << std::endl;
        std::cerr << "-- SAMPLING STOPPED --" << std::endl;
        std::cerr << std::endl;
    }

    /// @brief Compute the FFT size from the duration in the ringbuffer configuration.
    size_t fft_size() const {
        auto rbdur = this->ringbuffer_duration();
        // Prepare ringbuffer.
        auto min_rbsize = static_cast<size_t>(std::ceil(rbdur * this->rate_));
        // Exponent for the FFT size.
        auto exponent = static_cast<size_t>(std::ceil(std::log2(min_rbsize)));
        // Size of the sampler ringbuffer = correlation size.
        return size_t(1) << exponent;
    }

    /// @brief Ringbuffer duration in seconds.
    double ringbuffer_duration() const {
        return ::ssrx::config::read_duration_seconds(this->conf["ringbuffer"]["duration"]);
    }

    /// @brief Number of blocks in the ringbuffer.
    size_t ringbuffer_size() const {
        return this->conf["ringbuffer"]["nblocks"].template as<size_t>();
    }

    double sampling_rate() const {
        return this->rate_;
    }

    ::ssrx::RingBuffer<rb_data>& ringbuffer() {
        return this->rb;
    }

    const Configuration& config() const {
        return this->conf;
    }

    Configuration& config() {
        return this->conf;
    }

    zmq::context_t& zmq_context() {
        return this->ctx;
    }

    std::mutex* socket_log_mutex() {
        return &this->m_mtx;
    }
    
    /// @brief Synchronize time source with external PPS.
    ///        Absolute time refers to PC's clock.
    /// @param no_pps If set to true, computer clock is used and don't wait for the PPS.
    /// @return Return time at synchronization in time_t.
    virtual time_t synchronize(bool no_pps = false) {
        return time_t();
    }

    /// @brief Start sampling at designated time.
    /// @param when Time to start sampling.
    virtual void start_sampling(time_t when = 0) {
        if (when) {
            auto utc = std::gmtime(&when);
            std::cerr
                << std::endl
                << "\033[1;32mBegin streaming at " << when 
                << " (" << std::put_time(utc, "%Y/%m/%d %H:%M:%S UTC") << ") ...\033[0m" << std::endl;
        }
        socket = ::ssrx::mq::create_socket(this->ctx, this->conf["sampler"]["mq"], "Sampler", false, &this->m_mtx);
        auto send_timeout_ms = static_cast<int>(std::ceil(ringbuffer_duration() * ringbuffer_size() * 1000.0));
        if (send_timeout_ms < 100) {
            send_timeout_ms = 100;
        }
        socket.set(::zmq::sockopt::sndtimeo, send_timeout_ms);
    }

    /// @brief Store data read from SDR.
    /// @param data Pointer to the head of the data samples.
    /// @param size The number of samples.
    rb_data* store(const TimeSpec& timespec, raw_t* data, size_t num_rx_samps) {
        if (!is_running()) {
            return nullptr;
        }

        auto* packet = &this->rb.write_buffer().content();
        packet->add_time(timespec);

        size_t written;
        auto from = packet->copy_from(data, num_rx_samps, written);
        size_t remaining = num_rx_samps - written;

        // Check if we need to go to the next buffer.
        while (remaining != 0) {
            if (!is_running()) {
                return nullptr;
            }

            // Get the index of the current buffer.
            auto iread = this->rb.write_pointer();
            auto previous = packet;

            // Prepare the next buffer.
            this->rb.advance_write_pointer();

            // Look for the next available buffer.
            while (this->running && !this->rb.write_buffer().is_writable()) {
                this->rb.advance_write_pointer();
            }
            if (!is_running()) {
                return nullptr;
            }

            // Get the next packet.
            packet = &this->rb.write_buffer().content();
            packet->reset();
            
            // Copy the overlapped part from the previous buffer.
            packet->copy_from(previous->begin_overlapped(), previous->noverlapped(), written);

            // Copy remaining samples that could not be written in the previous buffer.
            from = packet->copy_from(from, remaining, written);
            remaining -= written;

            // Start offset of the fresh samples.
            packet->add_time(previous->time_at_overlap(this->rate_));

            // Send notification.
            if (!is_running()) {
                return nullptr;
            }
            ::zmq::message_t msg(&iread, sizeof(size_t));
            try {
                if (!socket.send(msg, ::zmq::send_flags::none)) {
                    std::cerr << "Error: zmq failed: sampler2correlator" << std::endl;
                    request_stop();
                    return nullptr;
                }
            } catch (const zmq::error_t& e) {
                if (is_running()) {
                    std::cerr << "Error: zmq failed: sampler2correlator: "
                              << e.what() << '[' << e.num() << ']' << std::endl;
                }
                request_stop();
                return nullptr;
            }
        }
        return packet;
    }

    /// @brief Wait until the sample start time to come.
    void wait_start_time(time_t when) {
        auto now = std::chrono::system_clock::now();
        auto target_time = std::chrono::system_clock::time_point(std::chrono::seconds(when));
        auto wake_time = target_time - std::chrono::milliseconds(100);

        if (wake_time > now) {
            std::this_thread::sleep_for(wake_time - now);
        } else {
            auto current_time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            std::ostringstream oss;
            oss << "Requested time " << when << " is behind or too close to the current time " << current_time;
            throw std::runtime_error(oss.str());
        }
    }

    /// @brief Start a thread. This thread can be joined
    /// @param thread 
    template<typename F, typename... Args>
    void start_thread(F&& functor, Args&&... args) {
        this->threads.emplace_back(std::forward<F>(functor), std::forward<Args>(args)...);
    }

    /// @brief Start a monitoring thread for the ringbuffer status.
    void start_monitor() {
        this->start_thread([&] {
            // Monitor interval.
            auto monitor_interval = ::ssrx::config::read_duration_seconds(
                conf["monitor"]["interval"], 10.0);

            // Watchdog timeout for ringbuffer updates.
            // If <= 0, watchdog is disabled.
            auto stalled_timeout = ::ssrx::config::read_duration_seconds(
                conf["monitor"]["stalled_timeout"], 0.0);

            auto socket = ::ssrx::mq::create_socket(ctx, conf["monitor"]["mq"], "Monitor", false, &m_mtx);

            auto numblk = rb.size();
            auto prev_wptr = rb.write_pointer();
            auto stalled_elapsed = 0.0;

            auto interval = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double>(monitor_interval));
            auto clock = std::chrono::system_clock::now();
            while (running) {
                auto now = std::chrono::system_clock::now();
                auto diff = std::chrono::duration_cast<std::chrono::microseconds>(now - clock);
                if (diff > interval) {
                    auto data = ::ssrx::monitor_bits::pack(
                        numblk,
                        [this](size_t i) { return rb[i].is_writable(); });
                    clock = now;
                    if (running) {
                        socket.send(zmq::message_t(data.data(), data.size()), zmq::send_flags::none);
                    }

                    auto curr_wptr = rb.write_pointer();
                    if (curr_wptr == prev_wptr) {
                        stalled_elapsed += monitor_interval;
                    } else {
                        stalled_elapsed = 0.0;
                        prev_wptr = curr_wptr;
                    }

                    if (stalled_timeout > 0.0 && stalled_elapsed >= stalled_timeout) {
                        std::cerr << "Error: ringbuffer update stalled for "
                                  << stalled_elapsed
                                  << " s (possible hardware/cable fault). aborting for restart." << std::endl;
                        std::abort();
                    }

                } else {
                    std::this_thread::sleep_for(interval - diff);
                }
            }

        });
    }

    /// @brief Start a dummy signal processing thread that checks the time and sample index integrity prepared by the sampler.
    void start_integrity_tester() {
        this->start_thread([&] {
            auto sock_rx = ::ssrx::mq::create_socket(ctx, conf["sampler"]["mq"], "Tester", true, &m_mtx);
            sock_rx.set(::zmq::sockopt::rcvtimeo, 100);
            
            int irecv = 0;
            rb_data* packet_prev = nullptr;
            std::cerr << std::endl;
            std::cerr << "-- INTEGRITY TESTER STARTED --" << std::endl << std::endl;

            while (running) {

                ::zmq::message_t recv;
                if (!sock_rx.recv(recv, ::zmq::recv_flags::none)) {
                    continue;
                }

                if (recv.size() != sizeof(size_t)) {
                    std::cerr << "Error: unexpected sampler message size: " << recv.size() << std::endl;
                    running = false;
                    break;
                }

                size_t iread = 0;
                std::memcpy(&iread, recv.data(), sizeof(iread));

                rb_data* input = &rb[iread].content();

                // Check that head and tail zero paddings.
                assert(std::all_of(
                    input->samples(),
                    input->begin(),
                    [](raw_t x) { return x == raw_t(); }));
                assert(std::all_of(
                    input->end_overlapped(),
                    input->samples() + input->fft_size(),
                    [](raw_t x) { return x == raw_t(); }));
                if (irecv == 0) {
                    // This should be close to "Time synchronized:" lines + 1 second.
                    std::cerr << "Time at first sample: " << input->time() << std::endl;
                    // assert(input->time_spec - stream_cmd.time_spec < 1e-6);
                } else {
                    // First zero region is skipped, and following overlapped elements are equal to the last same number of samples in the previous packet.
                    assert(std::equal(packet_prev->begin_overlapped(), packet_prev->end_overlapped(), input->begin()));
                    
                    // Start time of each packet has correct interval with each other.
                    auto ndiff = packet_prev->size() - packet_prev->noverlapped();
                    // std::cerr << 
                    auto tdiff = std::abs(static_cast<double>(input->time() - packet_prev->time()) - ndiff / rate_);
                    if (tdiff > 1e-12) {
                        std::cerr << std::chrono::system_clock::now().time_since_epoch().count() << std::endl;
                        std::cerr << input->time() << std::endl;
                        std::cerr << packet_prev->time() << std::endl;
                        std::cerr << (input->time() - packet_prev->time()) << std::endl;
                        std::cerr << ndiff << std::endl;
                        std::cerr << tdiff << std::endl;
                        throw std::runtime_error("Test failed");
                    }
                }
                packet_prev = input;
                irecv += 1;
                
                // Finish this buffer.
                rb[iread].set_state_writable();
            }
            
            std::cerr << std::endl;
            std::cerr << "-- All TESTS PASSED --" << std::endl;
            std::cerr << std::endl;
            
            std::cerr << std::endl;
            std::cerr << "-- INTEGRITY TESTER STOPPED --" << std::endl;
            std::cerr << std::endl;
        });
    }

    void start_amphist() {
        this->start_thread([&] {
            auto sock_rx = ::ssrx::mq::create_socket(ctx, conf["sampler"]["mq"], "AmpHist", true, &m_mtx);
            sock_rx.set(::zmq::sockopt::rcvtimeo, 100);

            AmpHist amphist;

            while (running) {
                ::zmq::message_t recv;
                if (!sock_rx.recv(recv, ::zmq::recv_flags::none)) {
                    continue;
                }

                if (recv.size() != sizeof(size_t)) {
                    std::cerr << "Error: unexpected sampler message size: " << recv.size() << std::endl;
                    running = false;
                    break;
                }

                size_t iread = 0;
                std::memcpy(&iread, recv.data(), sizeof(iread));

                rb_data* input = &rb[iread].content();

                input->convert();  // buffer[0] is now normalized to full_scale.
                amphist.draw_histogram(input->begin(0), input->fresh_size());
                if (amphist.is_stopping()) {
                    running = false;
                    break;
                }

                rb[iread].set_state_writable();
            }
            std::cerr << "-- DUMMY CONSUMER STOPPED --" << std::endl;
        });
    }

protected:

    /// @brief Configuration.
    Configuration conf;

    /// @brief A state which indicates running state.
    std::atomic_bool running;

    // ZMQ context for inproc queue among threads.
    zmq::context_t ctx;

    // ZMQ sampler socket.
    zmq::socket_t socket;

    std::mutex m_mtx;

    std::vector<std::thread> threads;

    /// @brief Ring buffer.
    ::ssrx::RingBuffer<rb_data> rb;

    /// @brief Actual sampling rate set to USRP.
    double rate_;
};

}}

namespace std {
template<class CharT, class Traits>
basic_ostream<CharT, Traits>& operator<<(basic_ostream<CharT, Traits>& os, const ssrx::sdr::TimeSpec& t) {
    os << t.seconds.count() << "." << std::setw(15) << std::setfill('0') << t.femtoseconds.count();
    return os;
}

}

namespace std {
template<typename Char, typename Traits>
basic_ostream<Char, Traits>& operator <<(basic_ostream<Char, Traits>& ost, const ssrx::sdr::Configuration& conf) {
    return (ost << conf.node());
}
}

#endif

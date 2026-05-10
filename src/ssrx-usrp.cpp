#include <csignal>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>
#include <deque>
#include <unordered_map>
#include <numeric>
#include <sstream>
#include <string>

#include <CLI/CLI.hpp>

#include "confighelper.hpp"
#include "usrp.hpp"
#include "ssrx.hpp"


volatile sig_atomic_t signal_stop_requested = 0;

void sig_handler(int)
{
    signal_stop_requested = 1;
}

int main(int argc, char* argv[]) {

    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        std::cerr << "Error in setting signal handler." << std::endl;
        return EXIT_FAILURE;
    }

    // ________________________________________________________________________
    // ````````````````````````````````````````````````````````````````````````
    //                           Program Arguments
    // ________________________________________________________________________
    // ````````````````````````````````````````````````````````````````````````

    bool test = false;
    bool amphist = false;
    bool test_integrity = false;
    bool no_pps = false;
    auto path_yaml = ssrx::config::expand_path("~/.ssrx/conf/ssrx.yaml");

    CLI::App app{"SSR receiver program using USRP device."};
    app.set_help_flag("-h,--help", "Shows help message.");
    app.set_version_flag(
        "-v,--version",
        std::to_string(VERSION_MAJOR) + "." +
        std::to_string(VERSION_MINOR) + "." +
        std::to_string(VERSION_PATCH),
        "Shows version number.");
    app.add_option("config", path_yaml, "Path to YAML configuration file.");
    app.add_flag("--test", test, "Test configuration and USRP, then exit.");
    app.add_flag("--test-integrity", test_integrity, "Test ringbuffer integrity.");
    app.add_flag("-a,--amphist", amphist, "Start amplitude histogram to check the signal quality.");
    app.add_flag("-n,--no-pps", no_pps, "Start sampling without latching time with the next PPS.");
    CLI11_PARSE(app, argc, argv);
    path_yaml = ssrx::config::expand_path(path_yaml);

    if (test) {
        std::cerr << std::endl;
        std::cerr << "Configuration file: " << path_yaml << std::endl;
        std::cerr << std::endl;
    }

    // ________________________________________________________________________
    // ````````````````````````````````````````````````````````````````````````
    //                            Configuration
    // ________________________________________________________________________
    // ````````````````````````````````````````````````````````````````````````

    using sample_raw_t = std::complex<double>;
    using sample_value_t = std::complex<float>;
    using ringbuffer_packet_t = ssrx::sdr::RingBufferData<sample_raw_t, sample_value_t>;
    using pipeline_data_t = ssrx::rb_data<ringbuffer_packet_t>;
    using sdr_device_t = ssrx::sdr::USRP<pipeline_data_t>;
    using receiver_t = ssrx::SSRX<sdr_device_t>;

    std::unique_ptr<receiver_t> sdr;

    try {
        ssrx::sdr::Configuration conf(path_yaml);
        auto usrp_conf = conf["usrp"];
        if (!usrp_conf || !usrp_conf.IsMap()) {
            throw std::runtime_error("missing required 'usrp' section in configuration");
        }
        sdr = std::make_unique<receiver_t>(std::move(conf));
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to initialize USRP receiver: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    time_t sync_time;
    try {
        sync_time = sdr->synchronize(no_pps);
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to synchronize time source: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    if (test) {
        return EXIT_SUCCESS;
    }

    // ________________________________________________________________________
    // ````````````````````````````````````````````````````````````````````````
    //                              Start threads
    // ________________________________________________________________________
    // ````````````````````````````````````````````````````````````````````````

    if (amphist) {
        sdr->start_amphist();
    } else if (test_integrity) {
        sdr->start_integrity_tester();
    } else {
        sdr->start_correlator();
        sdr->start_detector();
        sdr->start_demodulator();
        sdr->start_collector();
        sdr->start_printer();
    }

    sdr->start_monitor();
    sdr->start_sampling(sync_time);

    while (sdr->is_running()) {
        if (signal_stop_requested) {
            sdr->request_stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sdr->stop();

    return EXIT_SUCCESS;
}

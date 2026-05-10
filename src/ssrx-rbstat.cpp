#include <csignal>
#include <cerrno>
#include <clocale>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <cstring>
#include <CLI/CLI.hpp>
#include <zmq.hpp>
#include <yaml-cpp/yaml.h>

#include "confighelper.hpp"
#include "curses_compat.hpp"
#include "monitor_bits.hpp"


volatile sig_atomic_t running = 1;

void sig_handler(int)
{
    running = 0;
}

struct CursesGuard {
    bool active = true;

    void close() {
        if (active) {
            endwin();
            active = false;
        }
    }

    ~CursesGuard() {
        close();
    }
};

  
int main(int argc, char* argv[]) {

    setlocale(LC_ALL, "");

    auto path_yaml = ssrx::config::expand_path("~/.ssrx/conf/ssrx.yaml");

    CLI::App app{"Display ringbuffer state from monitor mq stream."};
    app.set_help_flag("-h,--help", "Shows help message.");
    app.set_version_flag(
        "-v,--version",
        std::to_string(VERSION_MAJOR) + "." +
        std::to_string(VERSION_MINOR) + "." +
        std::to_string(VERSION_PATCH),
        "Shows version number.");
    app.add_option("config", path_yaml, "Path to YAML configuration file.");
    CLI11_PARSE(app, argc, argv);
    path_yaml = ssrx::config::expand_path(path_yaml);

    std::string mq_addr;
    try {
        auto conf = YAML::LoadFile(path_yaml);
        mq_addr = conf["monitor"]["mq"]["addr"].as<std::string>();
    } catch (const std::exception& e) {
        std::cerr << "Error: failed to load monitor.mq.addr from conf: " << path_yaml << std::endl;
        std::cerr << "  " << e.what() << std::endl;
        return 1;
    }

    // Bind wildcard host is not connectable from another process.
    auto host_wildcard = mq_addr.find("*");
    if (host_wildcard != std::string::npos) {
        mq_addr.replace(host_wildcard, 1, "localhost");
    }

    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        std::cerr << "Error in setting signal handler." << std::endl;
        return 2;
    }

    auto window = initscr();
    if (!window) {
        std::cerr << "Error: failed to initialize ncurses." << std::endl;
        return 3;
    }
    CursesGuard curses_guard;
    curs_set(0);
    noecho();
    cbreak();
    keypad(window, true);
    start_color();

    zmq::context_t ctx;
    zmq::socket_t sock(ctx, zmq::socket_type::sub);
    sock.set(zmq::sockopt::linger, 0);
    sock.set(zmq::sockopt::rcvtimeo, 100);
    sock.set(zmq::sockopt::subscribe, "");
    sock.connect(mq_addr);

    // Error logs.
    std::ostringstream oss;

    const char* writable = " ";
    const char* readable = "█";

    int exit_code = EXIT_SUCCESS;
    while (running) {
        zmq::message_t recv;
        try {
            if (!sock.recv(recv, zmq::recv_flags::none)) {
                continue;
            }
        } catch (const zmq::error_t& e) {
            if (e.num() == EINTR) {
                if (!running) {
                    break;
                }
                continue;
            }
            oss << "Error: zmq recv failed: " << e.what() << " [" << e.num() << "]";
            running = false;
            exit_code = 4;
            break;
        }
        if (!running) {
            continue;
        }
        if (recv.size() < sizeof(size_t)) {
            continue;
        }

        size_t nbits = 0;
        std::memcpy(&nbits, recv.data(), sizeof(size_t));
        auto* bytes = static_cast<const uint8_t*>(recv.data()) + sizeof(size_t);  // byte blocks.
        size_t nblocks = recv.size() - sizeof(size_t);

        size_t ncols = COLS;
        size_t nrows = std::max<std::uint64_t>(1, nbits / ncols);
        if (nrows >= (size_t)LINES) {
            oss << "Error: lines must be > " << nrows;
            running = false;
            exit_code = 3;
            break;
        }

        werase(window);
        for (size_t i = 0; i < nrows; ++i) {
            bool done = false;
            for (size_t j = 0; j < ncols; ++j) {
                auto k = j + i * ncols;
                if (k < nbits) {
                    wmove(window, i, j);
                    waddstr(window, ssrx::monitor_bits::test(bytes, nblocks, k) ? writable : readable);
                } else if (!done) {
                    wmove(window, i, j);
                    waddstr(window, "|");
                    done = true;
                }
            }
        }
        auto nbits_str = std::to_string(nbits);
        std::ostringstream fmt;
        fmt << "%" << nbits_str.size() << "d/%s";
        wmove(window, nrows, 0);
        auto nwritable = ssrx::monitor_bits::count(bytes, nblocks, nbits);
        wprintw(window, fmt.str().c_str(), (int)(nbits - nwritable), nbits_str.c_str());
        wrefresh(window);
    }

    curses_guard.close();

    if (!oss.str().empty()) {
        std::cerr << oss.str() << std::endl;
    }

    return exit_code;
}

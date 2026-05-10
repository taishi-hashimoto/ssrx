#pragma once
#include <vector>
#include <complex>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include "curses_compat.hpp"

namespace ssrx {  namespace sdr {

struct AmpHist {
    bool auto_init = true;
    bool inited    = false;
    bool quit_requested = false;

    AmpHist() = default;
    ~AmpHist() {
        shutdown();
    }

    std::vector<double> max_hold;
    int cached_W = -1;
    int cached_bins = -1;

    void init_if_needed() {
#if SSRX_HAS_CURSES
        if (inited) return;
        if (auto_init) {
            initscr();
            cbreak();
            noecho();
            keypad(stdscr, TRUE);
            nodelay(stdscr, TRUE);
            curs_set(0);
        }
        inited = true;
#endif
    }

    void shutdown() {
#if SSRX_HAS_CURSES
        if (!inited) return;
        if (auto_init) endwin();
        inited = false;
        max_hold.clear();
        cached_W = cached_bins = -1;
#endif
    }

    bool is_stopping() {
        bool requested = quit_requested;
        quit_requested = false;
        return requested;
    }

    // full_scale is the maximum amplitude for the horizontal axis (example):
    //  - HackRF 8bit signed -> 127 (if still in sc8)
    //  - If normalized to floating point [-1,1] -> 1.0
    //  - 16bit signed -> 32767, etc.
    template<typename T>
    void draw_histogram(const std::complex<T>* data, std::size_t n,
                        double full_scale = 1.0,
                        bool reset_max=false) {
    #if !SSRX_HAS_CURSES
        (void)data;
        (void)n;
        (void)full_scale;
        (void)reset_max;
        return;
    #else
        init_if_needed();
        if (!data || n == 0) return;

        int H, W;
        getmaxyx(stdscr, H, W);
        int nbins = std::max(8, W - 2);

        if (H < 5 || nbins < 8) {
            erase();
            mvprintw(0, 0, "Terminal too small. Need H>=5, W>=10");
            refresh();
            return;
        }

        if (reset_max || nbins != cached_bins || W != cached_W || (int)max_hold.size() != nbins) {
            max_hold.assign(nbins, 0.0);
            cached_bins = nbins;
            cached_W = W;
        }

        const double eps = 1e-12;
        if (!(full_scale > eps)) full_scale = 1.0;

        // Binning（0..full_scale）
        std::vector<std::size_t> hist(nbins, 0);
        for (std::size_t i = 0; i < n; ++i) {
            double a = std::abs(data[i]);
            if (a < 0) a = 0;
            if (a > full_scale) a = full_scale;  // Clipping
            int b = static_cast<int>( (a / full_scale) * nbins );
            if (b >= nbins) b = nbins - 1;
            if (b < 0)      b = 0;
            ++hist[b];
        }

        // Normalization
        std::size_t count_max = 1;
        for (auto c : hist) if (c > count_max) count_max = c;

        std::vector<double> h_now(nbins, 0.0);
        for (int i = 0; i < nbins; ++i) {
            double v = static_cast<double>(hist[i]) / static_cast<double>(count_max);
            h_now[i] = v;
            if (v > max_hold[i]) max_hold[i] = v;
        }

        erase();
        mvprintw(0, 0,
                 "AmpHist  bins=%d  full_scale=%.6g  max_count=%zu  [q:quit r:reset]",
                 nbins, full_scale, count_max);
        // auto mean = std::accumulate(data, data+n, 0.0, [](double a, const std::complex<T>& b) {
        //     return a + std::abs(b);
        // }) / n;
        // auto vmax = std::abs(*std::max_element(data, data+n, [](const auto& a, const auto& b) {
        //     return std::abs(a) < std::abs(b);
        // }));
        // auto vmin = std::abs(*std::min_element(data, data+n, [](const auto& a, const auto& b) {
        //     return std::abs(a) < std::abs(b);
        // }));
        // mvprintw(1, nbins-12, "mean: %.3f", mean);
        // mvprintw(2, nbins-12, "max : %.3f", vmax);
        // mvprintw(3, nbins-12, "min : %.3f", vmin);

        int plot_top = 1;
        int plot_bottom = H - 2;
        int plot_h = std::max(1, plot_bottom - plot_top);

        // Draw bins.
        for (int i = 0; i < nbins; ++i) {
            int x = 1 + i;                 // left margin 1
            if (x >= W-1) break;           // right margin 1
            int h_now_px  = static_cast<int>(std::round(h_now[i]  * plot_h));
            int h_hold_px = static_cast<int>(std::round(max_hold[i]* plot_h));
            if (h_now_px  > plot_h)  h_now_px  = plot_h;
            if (h_hold_px > plot_h)  h_hold_px = plot_h;

            // current '|'
            for (int yy = 0; yy < h_now_px; ++yy) {
                int y = plot_bottom - yy;
                mvaddch(y, x, '|');
            }
            // max hold
            if (h_hold_px > 0) {
                int y = plot_bottom - (h_hold_px - 1);
                if (i == nbins - 1) {
                    mvaddch(y, x, 'x');
                } else {
                    mvaddch(y, x, '-');
                }
            }
        }

        mvprintw(H-1, 0, "Tip: right end (=full_scale) indicates saturation/clipping");
        refresh();

        int ch = getch();
        if (ch == 'r' || ch == 'R') {
            std::fill(max_hold.begin(), max_hold.end(), 0.0);
        } else if (ch == 'q' || ch == 'Q') {
            quit_requested = true;
        }
#endif
    }
};

} }  // ssrx::sdr

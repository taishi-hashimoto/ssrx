#include "confighelper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect_near(double actual, double expected) {
    auto tolerance = std::max(1.0, std::abs(expected)) * 1e-12;
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "Expected " << expected << ", got " << actual << std::endl;
        std::exit(1);
    }
}

void expect_equal(const std::string& actual, const std::string& expected) {
    if (actual != expected) {
        std::cerr << "Expected \"" << expected << "\", got \"" << actual << '"' << std::endl;
        std::exit(1);
    }
}

}

int main() {
    expect_near(ssrx::config::convert_unit(1090.0, "MHz"), 1090e6);
    expect_near(ssrx::config::convert_unit(50.0, "ms"), 0.05);
    expect_near(ssrx::config::convert_unit(1.0, "hour"), 3600.0);

    auto frequency = YAML::Load("{value: 12, unit: MHz}");
    expect_near(ssrx::config::read_frequency_hz(frequency), 12e6);

    auto duration = YAML::Load("{value: 250, unit: ms}");
    expect_near(ssrx::config::read_duration_seconds(duration), 0.25);

    YAML::Node missing;
    expect_near(ssrx::config::read_duration_seconds(missing, 10.0), 10.0);

    auto no_unit = YAML::Load("{value: 42}");
    expect_near(ssrx::config::read_frequency_hz(no_unit), 42.0);

    expect_equal(ssrx::config::expand_path("plain/path"), "plain/path");
    auto home = ssrx::config::home_directory();
    if (!home.empty()) {
        expect_equal(ssrx::config::expand_path("~/ssrx-test"), home + "/ssrx-test");
    }

    try {
        (void)ssrx::config::convert_unit(1.0, "furlong");
        std::cerr << "Unknown unit should throw." << std::endl;
        return 1;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}

#ifndef SSRX_CONFIGHELPER_HPP
#define SSRX_CONFIGHELPER_HPP

#include <cstdlib>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace ssrx { namespace config {

inline std::string home_directory() {
#ifdef _WIN32
    if (auto* userprofile = std::getenv("USERPROFILE")) {
        return userprofile;
    }

    std::string home;
    if (auto* homedrive = std::getenv("HOMEDRIVE")) {
        home = homedrive;
    }
    if (auto* homepath = std::getenv("HOMEPATH")) {
        home += homepath;
    }
    return home;
#else
    if (auto* home = std::getenv("HOME")) {
        return home;
    }
    return {};
#endif
}

inline bool is_env_name_start(char c) {
    auto ch = static_cast<unsigned char>(c);
    return std::isalpha(ch) || c == '_';
}

inline bool is_env_name_char(char c) {
    auto ch = static_cast<unsigned char>(c);
    return std::isalnum(ch) || c == '_';
}

inline std::string expand_environment_variables(const std::string& path) {
    std::string result;
    result.reserve(path.size());

    for (size_t i = 0; i < path.size();) {
        if (path[i] != '$' || i + 1 >= path.size()) {
            result.push_back(path[i++]);
            continue;
        }

        if (path[i + 1] == '{') {
            auto end = path.find('}', i + 2);
            if (end == std::string::npos) {
                result.push_back(path[i++]);
                continue;
            }

            auto name = path.substr(i + 2, end - i - 2);
            if (auto* value = std::getenv(name.c_str())) {
                result += value;
            } else {
                result.append(path, i, end - i + 1);
            }
            i = end + 1;
            continue;
        }

        if (!is_env_name_start(path[i + 1])) {
            result.push_back(path[i++]);
            continue;
        }

        size_t end = i + 2;
        while (end < path.size() && is_env_name_char(path[end])) {
            ++end;
        }

        auto name = path.substr(i + 1, end - i - 1);
        if (auto* value = std::getenv(name.c_str())) {
            result += value;
        } else {
            result.append(path, i, end - i);
        }
        i = end;
    }

    return result;
}

inline std::string expand_path(std::string path) {
    if (!path.empty() && path[0] == '~' && (path.size() == 1 || path[1] == '/' || path[1] == '\\')) {
        auto home = home_directory();
        if (!home.empty()) {
            path.replace(0, 1, home);
        }
    }
    return expand_environment_variables(path);
}

class Configuration {
public:
    explicit Configuration(const std::string& path)
        : conf(YAML::LoadFile(expand_path(path)))
    { }

    template <typename Key>
    const YAML::Node operator[](const Key& key) const {
        return conf[key];
    }

    template <typename Key>
    YAML::Node operator[](const Key& key) {
        return conf[key];
    }

    const YAML::Node& node() const {
        return conf;
    }

private:
    YAML::Node conf;
};

inline double unit_multiplier(std::string_view unit) {
    if (unit.empty()) { return 1.0; }

    if (unit == "THz") { return 1e12; }
    if (unit == "GHz") { return 1e9; }
    if (unit == "MHz") { return 1e6; }
    if (unit == "kHz") { return 1e3; }
    if (unit == "Hz") { return 1.0; }

    if (unit == "s" || unit == "sec" || unit == "second" || unit == "seconds") { return 1.0; }
    if (unit == "ms") { return 1e-3; }
    if (unit == "us") { return 1e-6; }
    if (unit == "min" || unit == "minute" || unit == "minutes") { return 60.0; }
    if (unit == "hour" || unit == "hours") { return 3600.0; }

    throw std::invalid_argument("Unknown unit: " + std::string(unit));
}

inline double convert_unit(
    double value,
    std::string_view unit_from,
    std::string_view unit_to = "")
{
    return value * unit_multiplier(unit_from) / unit_multiplier(unit_to);
}

inline double read_value_with_unit(
    const YAML::Node& node,
    std::string_view default_unit = "")
{
    auto value = node["value"].as<double>();
    auto unit = node["unit"].as<std::string>(std::string(default_unit));
    return convert_unit(value, unit);
}

inline double read_value_with_unit(
    const YAML::Node& node,
    double default_value,
    std::string_view default_unit = "")
{
    auto value = node["value"].as<double>(default_value);
    auto unit = node["unit"].as<std::string>(std::string(default_unit));
    return convert_unit(value, unit);
}

inline double read_frequency_hz(const YAML::Node& node) {
    return read_value_with_unit(node, "Hz");
}

inline double read_duration_seconds(const YAML::Node& node) {
    return read_value_with_unit(node, "second");
}

inline double read_duration_seconds(
    const YAML::Node& node,
    double default_value,
    std::string_view default_unit = "second")
{
    return read_value_with_unit(node, default_value, default_unit);
}

} }

#endif

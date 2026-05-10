#ifndef SSRX_MQHELPER_HPP
#define SSRX_MQHELPER_HPP

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <zmq.hpp>
#include <yaml-cpp/yaml.h>


namespace ssrx { namespace mq {

namespace detail {

inline std::string to_lower(std::string str) {
    std::transform(
        str.begin(),
        str.end(),
        str.begin(),
        [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return str;
}

inline ::zmq::socket_type parse_zmq_type(std::string mq_type_str) {
    mq_type_str = to_lower(mq_type_str);
    if (mq_type_str == "push") {
        return ::zmq::socket_type::push;
    } else if (mq_type_str == "pull") {
        return ::zmq::socket_type::pull;
    } else if (mq_type_str == "pub") {
        return ::zmq::socket_type::pub;
    } else if (mq_type_str == "sub") {
        return ::zmq::socket_type::sub;
    } else {
        std::ostringstream oss;
        oss << "Error: Unknown socket_type: " << mq_type_str;
        throw std::runtime_error(oss.str());
    }
}

using zmq_method = void (zmq::socket_t::*)(const std::string&);

inline zmq_method parse_zmq_method(std::string mq_method_str) {
    mq_method_str = to_lower(mq_method_str);
    if (mq_method_str == "connect") {
        return &::zmq::socket_t::connect;
    } else if (mq_method_str == "bind") {
        return &::zmq::socket_t::bind;
    } else {
        throw std::runtime_error("Unknown ZeroMQ method: " + mq_method_str);
    }
}

inline std::string other_side_method(std::string method) {
    method = to_lower(method);
    if (method == "connect") {
        return "bind";
    } else if (method == "bind") {
        return "connect";
    } else {
        throw std::runtime_error("Unknown ZeroMQ method: " + method);
    }
}

inline std::string other_side_type(std::string mq_type) {
    mq_type = to_lower(mq_type);
    if (mq_type == "push") {
        return "pull";
    } else if (mq_type == "pull") {
        return "push";
    } else if (mq_type == "pub") {
        return "sub";
    } else if (mq_type == "sub") {
        return "pub";
    } else {
        throw std::runtime_error("Unknown ZeroMQ socket type: " + mq_type);
    }
}

}  // namespace detail

    /// @brief Create a ZeroMQ socket.
    /// @param ctx ZeroMQ context.
    /// @param addr Socket address (e.g., "tcp://*:5555", "ipc:///tmp/sock").
    /// @param type Socket type (e.g., "pub", "sub", "push", "pull").
    /// @param method Socket connection method (e.g., "bind", "connect").
    /// @param name Service's name.
    /// @param client If true, the socket is for the client side.
    /// @param mtx If specified, debug message is printed.
    /// @return The created ZeroMQ socket.
    inline zmq::socket_t create_socket(
        zmq::context_t& ctx,
        std::string addr,
        std::string type,
        std::string method,
        const std::string& name = "",
        bool client = false,
        std::mutex* mtx = nullptr
    ) {
        auto direction = "-->";

        type = detail::to_lower(type);
        method = detail::to_lower(method);

        if (client) {
            method = detail::other_side_method(method);
            type = detail::other_side_type(type);
            direction = "<--";
            auto idx_aster = addr.find('*');
            if (idx_aster != std::string::npos) {
                addr.replace(idx_aster, 1, "localhost");
            }
        }
        auto mq_type = detail::parse_zmq_type(type);
        auto mq_method = detail::parse_zmq_method(method);
        if (!name.empty() && mtx) {
            std::lock_guard<std::mutex> lock(*mtx);
            std::cerr
                << direction << " \"" << addr << "\" (" << type << ", " << method << "): \033[1;33m" << name << " 0MQ\033[0m" << std::endl;
        }

        // Prepare message queue.
        try {
            ::zmq::socket_t socket(ctx, mq_type);
            socket.set(::zmq::sockopt::linger, 0);
            (socket.*mq_method)(addr);
            if (type == "sub") {
                socket.set(::zmq::sockopt::subscribe, "");
            }
            return socket;
        } catch (const zmq::error_t& e) {
            std::cerr << "Error: " << e.what() << '[' << e.num() << ']' << std::endl;
            throw;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            throw;
        }
    }

    /// @brief Create a ZeroMQ socket.
    /// @param mqspec `YAML::Node` for mq configuration.
    /// @param name Optional name for messages displayed onto the standard output.
    /// @param client If true, a socket which is to be connected (client side).
    inline zmq::socket_t create_socket(
        zmq::context_t& ctx,
        const YAML::Node& mqspec,
        const std::string& name = "",
        bool client = false,
        std::mutex* mtx = nullptr
    ) {
        auto mq_tstr = mqspec["type"].template as<std::string>();
        auto mq_method_name = mqspec["method"].template as<std::string>();
        auto mq_addr = mqspec["addr"].template as<std::string>();

        return create_socket(ctx, mq_addr, mq_tstr, mq_method_name, name, client, mtx);
    }

    inline zmq::socket_t create_socket(
        zmq::context_t& ctx,
        const YAML::Node& mqspec,
        const std::string& name = "",
        bool client = false
    ) {
        return create_socket(ctx, mqspec, name, client, nullptr);
    }

    inline zmq::socket_t create_socket(
        zmq::context_t& ctx,
        const YAML::Node& mqspec,
        const char* name,
        bool client = false
    ) {
        return create_socket(ctx, mqspec, std::string(name), client, nullptr);
    }

    inline zmq::socket_t create_socket(
        zmq::context_t& ctx,
        const YAML::Node& mqspec,
        size_t index,
        const std::string& name = "",
        bool client = false,
        std::mutex* mtx = nullptr
    ) {
        auto mq_tstr = mqspec["type"].template as<std::string>();
        auto mq_method_name = mqspec["method"].template as<std::string>();
        auto mq_addr = mqspec["addr"].template as<std::string>();

        return create_socket(
            ctx,
            mq_addr+std::to_string(index),
            mq_tstr,
            mq_method_name,
            (name.empty() ? std::string() : name + std::to_string(index)),
            client, mtx);
    }

    inline zmq::socket_t create_socket(
        zmq::context_t& ctx,
        const YAML::Node& mqspec,
        size_t index,
        const std::string& name = "",
        bool client = false
    ) {
        return create_socket(ctx, mqspec, index, name, client, nullptr);
    }
} }

#endif // SSRX_MQHELPER_HPP

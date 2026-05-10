#include "mqhelper.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include <zmq.hpp>
#include <yaml-cpp/yaml.h>

namespace {

void expect_equal(const std::string& actual, const std::string& expected) {
    if (actual != expected) {
        std::cerr << "Expected \"" << expected << "\", got \"" << actual << '"' << std::endl;
        std::exit(1);
    }
}

}

int main() {
    zmq::context_t ctx;
    auto mqspec = YAML::Load("{type: push, method: bind, addr: 'inproc://ssrx-test-mqhelper'}");

    auto sender = ssrx::mq::create_socket(ctx, mqspec, "Sender", false);
    auto receiver = ssrx::mq::create_socket(ctx, mqspec, "Receiver", true);
    receiver.set(zmq::sockopt::rcvtimeo, 1000);

    const std::string payload = "hello mq";
    sender.send(zmq::buffer(payload), zmq::send_flags::none);

    zmq::message_t message;
    auto received = receiver.recv(message, zmq::recv_flags::none);
    if (!received) {
        std::cerr << "Expected to receive a message." << std::endl;
        return 1;
    }

    expect_equal(
        std::string(static_cast<const char*>(message.data()), message.size()),
        payload);

    return 0;
}

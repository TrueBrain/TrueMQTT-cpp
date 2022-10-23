/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <TrueMQTT.h>

#include <iostream>
#include <magic_enum.hpp>
#include <thread>

int main()
{
    // Create a connection to the local broker.
    TrueMQTT::Client client("localhost", 1883, "test");

    client.setLogger(
        TrueMQTT::Client::LogLevel::WARNING,
        [](TrueMQTT::Client::LogLevel level, std::string_view message)
        {
            std::cout << "Log " << std::string(magic_enum::enum_name(level)) << ": " << message << std::endl;
        });
    client.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 10);
    client.setErrorCallback(
        [](TrueMQTT::Client::Error error, std::string_view message)
        {
            std::cout << "Error " << std::string(magic_enum::enum_name(error)) << ": " << message << std::endl;
        });
    client.setLastWill("example/pubsub/lastwill", "example pubsub finished", true);

    client.connect();

    int stop = 0;

    // Subscribe to the topic we will be publishing under in a bit.
    client.subscribe(
        "example/pubsub/test/subtest",
        [&stop](const std::string_view topic, const std::string_view payload)
        {
            std::cout << "Received message on exact topic " << topic << ": " << payload << std::endl;
            stop++;
        });
    client.subscribe(
        "example/pubsub/test/subtest",
        [&stop](const std::string_view topic, const std::string_view payload)
        {
            std::cout << "Received message on exact topic " << topic << " again: " << payload << std::endl;
            stop++;
        });
    client.subscribe(
        "example/pubsub/+/subtest",
        [&stop](const std::string_view topic, const std::string_view payload)
        {
            std::cout << "Received message on single wildcard topic " << topic << ": " << payload << std::endl;
            stop++;
        });
    client.subscribe(
        "example/pubsub/test/#",
        [&stop](const std::string_view topic, const std::string_view payload)
        {
            std::cout << "Received message on multi wildcard topic " << topic << ": " << payload << std::endl;
            stop++;
        });
    client.subscribe(
        "example/pubsub/test/+",
        [&stop](const std::string_view topic, const std::string_view payload)
        {
            /* Never actually called, as we unsubscribe a bit later */
        });

    client.unsubscribe("example/pubsub/test/+");

    // Publish a message on the same topic as we subscribed too.
    client.publish("example/pubsub/test/subtest", "Hello World!", false);

    // Wait till we receive the message back on our subscription.
    while (stop < 4)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.disconnect();

    return 0;
}

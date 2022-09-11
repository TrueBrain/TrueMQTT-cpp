/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <TrueMQTT.h>
#include <iostream>
#include <thread>

int main()
{
    // Create a connection to the local broker.
    TrueMQTT::Client client("localhost", 1883, "test");

    client.setLogger(TrueMQTT::Client::LogLevel::TRACE, [](TrueMQTT::Client::LogLevel level, std::string message)
                     { std::cout << "Log " << level << ": " << message << std::endl; });
    client.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 10);
    client.setErrorCallback([](TrueMQTT::Client::Error error, std::string message)
                            { std::cout << "Error " << error << ": " << message << std::endl; });

    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int stop = 0;

    // Subscribe to the topic we will be publishing under in a bit.
    client.subscribe("test/test/test", [&stop](const std::string topic, const std::string payload)
                     {
        std::cout << "Received message on exact topic " << topic << ": " << payload << std::endl;
        stop++; });
    client.subscribe("test/+/test", [&stop](const std::string topic, const std::string payload)
                     {
        std::cout << "Received message on single wildcard topic " << topic << ": " << payload << std::endl;
        stop++; });
    client.subscribe("test/#", [&stop](const std::string topic, const std::string payload)
                     {
        std::cout << "Received message on multi wildcard topic " << topic << ": " << payload << std::endl;
        stop++; });
    client.subscribe("test/test/+", [&stop](const std::string topic, const std::string payload)
                     {
        /* Never actually called */ });

    client.unsubscribe("test/test/+");

    // Publish a message on the same topic as we subscribed too.
    client.publish("test/test/test", "Hello World!", false);

    // Wait till we receive the message back on our subscription.
    while (stop != 3)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.disconnect();

    return 0;
}

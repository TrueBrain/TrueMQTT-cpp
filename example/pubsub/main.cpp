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

    bool stop = false;

    // Subscribe to the topic we will be publishing under in a bit.
    client.subscribe("test", [&stop](const std::string &topic, const std::string &payload)
                     {
        std::cout << "Received message on topic " << topic << ": " << payload << std::endl;
        stop = true; });

    // Publish a message on the same topic as we subscribed too.
    client.publish("test", "Hello World!", false);

    // Wait till we receive the message back on our subscription.
    while (!stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.disconnect();

    return 0;
}

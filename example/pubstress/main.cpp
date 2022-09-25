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

    client.setLogger(TrueMQTT::Client::LogLevel::WARNING, [](TrueMQTT::Client::LogLevel level, std::string message)
                     { std::cout << "Log " << level << ": " << message << std::endl; });
    client.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 100);
    client.setErrorCallback([](TrueMQTT::Client::Error error, std::string message)
                            { std::cout << "Error " << error << ": " << message << std::endl; });
    client.setLastWill("test/lastwill", "example pubsub finished", true);

    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int stop = 0;

    // Subscribe to the topic we are going to stress test.
    client.subscribe("test/test/test", [&stop](const std::string topic, const std::string payload) {});

    // Send a lot of packets constantly, while telling us when publishing is failing.
    // The expected behaviour is that this goes okay for a while, till the broker
    // backs up, after which it starts to fail intermittently. To push the broker
    // to its breaking point, it helps to add additional subscriptions by other
    // means.
    bool is_failing = true;
    while (true)
    {
        if (!client.publish("test/test/test", "Hello World!", false))
        {
            if (!is_failing)
            {
                is_failing = true;
                std::cout << "Failed to publish message" << std::endl;
            }
        }
        else
        {
            if (is_failing)
            {
                is_failing = false;
                std::cout << "Succeeded to publish message" << std::endl;
            }
        }
    }

    // This application never ends, but for good measure, a disconnect.
    client.disconnect();

    return 0;
}

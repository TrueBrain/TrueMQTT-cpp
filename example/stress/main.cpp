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

    client.setLogger(TrueMQTT::Client::LogLevel::WARNING, [](TrueMQTT::Client::LogLevel level, std::string_view message)
                     { std::cout << "Log " << level << ": " << message << std::endl; });
    client.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 100);
    client.setErrorCallback([](TrueMQTT::Client::Error error, std::string_view message)
                            { std::cout << "Error " << error << ": " << message << std::endl; });
    client.setLastWill("test/lastwill", "example pubsub finished", true);

    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    size_t received = 0;
    size_t sent = 0;
    size_t failed = 0;
    int64_t totalLatency = 0;

    // Subscribe to the topic we are going to stress test.
    client.subscribe("example/stress/+", [&received, &totalLatency](const std::string_view topic, const std::string_view payload)
                     {
        // Calculate the latency.
        auto now = std::chrono::steady_clock::now();
        auto then = std::chrono::time_point<std::chrono::steady_clock>(std::chrono::microseconds(std::stoll(std::string(payload))));
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(now - then).count();

        totalLatency += latency;
        received++; });

    // Send a lot of packets constantly, while telling us when publishing is failing.
    // The expected behaviour is that this goes okay for a while, till the broker
    // backs up, after which it starts to fail intermittently. To push the broker
    // to its breaking point, it helps to add additional subscriptions by other
    // means.
    bool is_failing = true;
    auto start = std::chrono::steady_clock::now();
    int channel = 0;
    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        // Publish the current time, so we can check the latency. We distribute
        // it over multiple topics, to give brokers a chance to distrubte the
        // load on their side.
        channel = (channel + 1) % 10;
        if (!client.publish("example/stress/" + std::to_string(channel), std::to_string(now_ms), false))
        {
            failed++;
        }
        else
        {
            sent++;
        }

        // Every second, tell how much messages per second we sent, received and failed.
        if (now - start > std::chrono::seconds(1))
        {
            if (received != 0)
            {
                std::cout << "Sent: " << sent << "/s - Received: " << received << "/s - Failed: " << failed << "/s - Avg Latency: " << (totalLatency / received) << "us" << std::endl;
            }
            else
            {
                std::cout << "Sent: " << sent << "/s - Received: " << received << "/s - Failed: " << failed << "/s " << std::endl;
            }
            sent = 0;
            received = 0;
            failed = 0;
            totalLatency = 0;
            start = now;
        }

        // Don't go too fast, to get a better idea of the latency.
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    // This application never ends, but for good measure, a disconnect.
    client.disconnect();

    return 0;
}

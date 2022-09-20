/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <TrueMQTT.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <vector>


enum TIME_RES
{
    T_MICRO,
    T_MILLI,
    T_SECONDS
};

std::vector<int> vecTimings;

std::uint64_t getEpochUSecs()
{
    auto tsUSecs = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(tsUSecs.time_since_epoch().count());
}

void processTimes(int epochTat)
{
    vecTimings.emplace_back( epochTat );

    if(vecTimings.size() == 10)
    {
        float avgTat = 0;
        int totalTat = 0;
        for(auto timing : vecTimings)
        {
            totalTat += timing;
        }
        avgTat = totalTat / vecTimings.size();

        std::cout << "Average : " << std::to_string(static_cast<int>(avgTat)) << " uSecs" << "( " << std::to_string( 1000000 / avgTat ) << " messages / second )" << std::endl;

        // Clear the vector
        vecTimings.clear();
    }
}

int main(int argc, char* argv[])
{
    // Create a connection to a local broker over IPv4 and setup the MQTT-Stack
    // =========================================================================
    TrueMQTT::Client client("127.0.0.1", 1883, "SpeedTestSub");

    client.setLogger(TrueMQTT::Client::LogLevel::TRACE, [](TrueMQTT::Client::LogLevel level, std::string message)
    { std::cout << "Log " << level << ": " << message << std::endl;});

    client.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 10);

    client.setErrorCallback([](TrueMQTT::Client::Error error, std::string message)
    { std::cout << "Error " << error << ": " << message << std::endl;});

    client.setLastWill("SpeedTest/lastWillSub", "SpeedTest Subscriber finished", true);

    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // =========================================================================
    // 
    // Keep receiving until we press ctrl-c

    client.subscribe("SpeedTest/#", [](const std::string topic, const std::string payload)
                     {
                            char *end;
                            int tat = getEpochUSecs() - std::strtoull(payload.c_str(), &end, 10);
                            processTimes(tat);
                     });

    while(1)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

}

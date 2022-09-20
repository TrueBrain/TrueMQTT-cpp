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

enum TIME_RES
{
    T_MICRO,
    T_MILLI,
    T_SECONDS
};

std::uint64_t getEpochUSecs()
{
    auto tsUSecs = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(tsUSecs.time_since_epoch().count());
}

int main(int argc, char* argv[])
{
    // Create a connection to a local broker over IPv4 and setup the MQTT-Stack
    // =========================================================================
    TrueMQTT::Client client("127.0.0.1", 1883, "SpeedTestPub");

    client.setLogger(TrueMQTT::Client::LogLevel::TRACE, [](TrueMQTT::Client::LogLevel level, std::string message)
    { std::cout << "Log " << level << ": " << message << std::endl;});

    client.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 10);

    client.setErrorCallback([](TrueMQTT::Client::Error error, std::string message)
    { std::cout << "Error " << error << ": " << message << std::endl;});

    client.setLastWill("SpeedTest/lastWill", "SpeedTest Publisher finished", true);

    client.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // =========================================================================
    // 
    // Keep sending until we press ctrl-c

    // Clear the SpeedTest topic before blowing the fuses
    client.publish("SpeedTest/", "", false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    unsigned int message_number = 0;
    while(1)
    {
        if(message_number < 10)
        {
            client.publish("SpeedTest/message[" + std::to_string(message_number) + "]", std::to_string(getEpochUSecs()), false);
            message_number++;
        }
        else
        {
            message_number = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

}

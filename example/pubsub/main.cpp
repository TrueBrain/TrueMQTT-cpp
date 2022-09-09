/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <TrueMQTT.h>
#include <iostream>

int main()
{
    // Create a connection to the local broker.
    TrueMQTT::Client client("localhost", 1883, "test");

    client.setLogger(TrueMQTT::Client::LogLevel::TRACE, [](TrueMQTT::Client::LogLevel level, std::string message) {
        std::cout << "Log " << level << ": " << message << std::endl;
    });

    client.connect();

    // Subscribe to the topic we will be publishing under in a bit.
    client.subscribe("test", [](const std::string &topic, const std::string &payload) {
        std::cout << "Received message on topic " << topic << ": " << payload << std::endl;
    });

    // Publish a message on the same topic as we subscribed too.
    client.publish("test", "Hello World!", false);

    client.disconnect();

    return 0;
}

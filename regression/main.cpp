/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <TrueMQTT.h>

#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <thread>

#include <iostream>

TEST_CASE("regression", "[regression]")
{
    // last_message / logger_mutex needs to outlive client1, as it is captured in a lambda.
    std::string last_message = {};
    std::mutex logger_mutex;

    TrueMQTT::Client client1("localhost", 1883, "client-1");
    TrueMQTT::Client client2("localhost", 1883, "client-2");

    client1.setLogger(
        TrueMQTT::Client::LogLevel::INFO,
        [&last_message, &logger_mutex](TrueMQTT::Client::LogLevel level, const std::string_view message)
        {
            std::scoped_lock lock(logger_mutex);
            last_message = message;
        });

    client2.setPublishQueue(TrueMQTT::Client::PublishQueueType::FIFO, 10);

    uint8_t connected = 0x0;
    client1.setStateChangeCallback(
        [&connected](TrueMQTT::Client::State state)
        {
            if (state == TrueMQTT::Client::State::CONNECTED)
            {
                connected |= 0x1;
            }
        });
    client2.setStateChangeCallback(
        [&connected](TrueMQTT::Client::State state)
        {
            if (state == TrueMQTT::Client::State::CONNECTED)
            {
                connected |= 0x2;
            }
        });

    // TC: Cannot publish/subscribe/unsubscribe when disconnected
    {
        CHECK(client1.publish("regression/topic1", "Hello World!", false) == false);
        CHECK(last_message == "Cannot publish when disconnected");

        client1.subscribe("regression/topic1", [](const std::string_view topic, const std::string_view message) { /* empty */ });
        CHECK(last_message == "Cannot subscribe when disconnected");

        client1.unsubscribe("regression/topic1");
        CHECK(last_message == "Cannot unsubscribe when disconnected");
    }

    uint8_t received = 0;
    uint8_t received2 = 0;

    // TC: Connect the subscribing client first and instantly make a subscription; subscription should be created after connect.
    {
        client1.connect();
        CHECK(last_message == "Connecting to localhost:1883");
        client1.subscribe(
            "regression/topic1",
            [&received](const std::string_view topic, const std::string_view payload)
            {
                CHECK(std::string(topic) == "regression/topic1");
                received++;
            });

        auto start = std::chrono::steady_clock::now();
        while ((connected & 0x1) == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            {
                FAIL("Timeout waiting for client to connect");
            }
        }

        // Give some time for the subscription to actually be made.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // TC: Connect the publishing client next and instantly publish a message; message should be published after connect.
    {
        client2.connect();
        CHECK(client2.publish("regression/topic1", "Hello World!", false) == true);

        auto start = std::chrono::steady_clock::now();
        while ((connected & 0x2) == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
            {
                FAIL("Timeout waiting for client to connect");
            }
        }
    }

    // TC: Publish before connect happened and check message arrives after connection is made
    {
        // Wait for the pending publish to arrive.
        auto start = std::chrono::steady_clock::now();
        while (received != 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
            {
                FAIL("Timeout waiting for message to arrive");
            }
        }
        received = 0;
    }

    // TC: Cannot connect when already connected
    {
        client1.connect();
        CHECK(last_message == "Can't connect when already connecting / connected");
    }

    // TC: Publish to delayed subscription must work
    {
        CHECK(client2.publish("regression/topic1", "Hello World!", false) == true);
        CHECK(client2.publish("regression/topic1", "Hello World!", false) == true);
        CHECK(client2.publish("regression/topic1", "Hello World!", false) == true);
        CHECK(client2.publish("regression/topic1", "Hello World!", false) == true);

        auto start = std::chrono::steady_clock::now();
        while (received != 4)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
            {
                FAIL("Timeout waiting for message to arrive");
            }
        }
        received = 0;
    }

    // TC: Subscribe to new topic and publish on it
    {
        client1.subscribe(
            "regression/topic2",
            [&received2](const std::string_view topic, const std::string_view payload)
            {
                CHECK(std::string(topic) == "regression/topic2");
                received2++;
            });

        // Give some time for the subscription to actually be made.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        CHECK(client2.publish("regression/topic2", "Hello World!", false) == true);

        // Wait for the pending publish to arrive.
        auto start = std::chrono::steady_clock::now();
        while (received2 != 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
            {
                FAIL("Timeout waiting for message to arrive");
            }
        }
        received2 = 0;
    }

    // TC: Unsubscribe and check a publish to the topic no longer arrives
    {
        client1.unsubscribe("regression/topic1");
        client1.unsubscribe("regression/topic2");

        CHECK(client2.publish("regression/topic1", "Hello World!", false) == true);
        CHECK(client2.publish("regression/topic2", "Hello World!", false) == true);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(received == 0);
        REQUIRE(received2 == 0);
    }

    // TC: Check wildcard subscriptions
    {
        client1.subscribe(
            "regression/topic3/+",
            [&received](const std::string_view topic, const std::string_view payload)
            {
                CHECK(std::string(topic) == "regression/topic3/1");
                received++;
            });
        client1.subscribe(
            "regression/#",
            [&received2](const std::string_view topic, const std::string_view payload)
            {
                if (received2 == 0)
                {
                    CHECK(std::string(topic) == "regression/topic3/1");
                }
                else
                {
                    CHECK(std::string(topic) == "regression/topic4/2");
                }
                received2++;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        CHECK(client2.publish("regression/topic3/1", "Hello World!", false) == true);
        CHECK(client2.publish("regression/topic4/2", "Hello World!", false) == true);

        auto start = std::chrono::steady_clock::now();
        while (received != 1 || received2 != 2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
            {
                FAIL("Timeout waiting for message to arrive");
            }
        }

        received = 0;
        received2 = 0;
    }

    // TC: Cannot unsubscribe from topic not subscribed to
    {
        client1.unsubscribe("regression/topic1");
        CHECK(last_message == "Cannot unsubscribe from topic 'regression/topic1' because we are not subscribed to it");
    }

    // TC: Validate disconnect is seen
    {
        client1.disconnect();
        CHECK(last_message == "Disconnecting from broker");
        client2.disconnect();
    }

    // TC: Cannot disconnect when not connected
    {
        client1.disconnect();
        CHECK(last_message == "Can't disconnect when already disconnected");
    }

    // TC: Connect to a non-existing broker
    {
        TrueMQTT::Client client3("localhost", 1884, "client-3");

        client3.setLogger(
            TrueMQTT::Client::LogLevel::INFO,
            [&last_message, &logger_mutex](TrueMQTT::Client::LogLevel level, const std::string_view message)
            {
                std::scoped_lock lock(logger_mutex);
                last_message = message;
            });
        client3.setStateChangeCallback(
            [&connected](TrueMQTT::Client::State state)
            {
                if (state == TrueMQTT::Client::State::CONNECTED)
                {
                    connected |= 0x4;
                }
            });

        client3.connect();

        CHECK(last_message == "Connecting to localhost:1884");

        bool failed = false;
        auto start = std::chrono::steady_clock::now();
        while ((connected & 0x4) == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100))
            {
                failed = true;
                break;
            }
        }
        REQUIRE(failed == true);

        client3.disconnect();
        CHECK(last_message == "Disconnecting from broker");
    }
}

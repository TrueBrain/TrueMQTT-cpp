/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "TrueMQTT.h"

#include "Connection.h"

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// This class tracks all internal variables of the client. This way the header
// doesn't need to include the internal implementation of the Client.
class TrueMQTT::Client::Impl
{
public:
    Impl(const std::string &host, int port, const std::string &client_id, int connection_timeout, int connection_backoff_max, int keep_alive_interval)
        : host(host),
          port(port),
          client_id(client_id),
          connection_timeout(connection_timeout),
          connection_backoff_max(connection_backoff_max),
          keep_alive_interval(keep_alive_interval)
    {
    }

    enum State
    {
        DISCONNECTED, ///< The client is not connected to the broker, nor is it trying to connect.
        CONNECTING,   ///< The client is either connecting or reconnecting to the broker. This can be in any stage of the connection process.
        CONNECTED,    ///< The client is connected to the broker.
    };

    void connect();                                                                         ///< Connect to the broker.
    void disconnect();                                                                      ///< Disconnect from the broker.
    void sendPublish(const std::string &topic, const std::string &payload, bool retain);    ///< Send a publish message to the broker.
    void sendSubscribe(const std::string &topic);                                           ///< Send a subscribe message to the broker.
    void sendUnsubscribe(const std::string &topic);                                         ///< Send an unsubscribe message to the broker.
    void connectionStateChange(bool connected);                                             ///< Called when a connection goes from CONNECTING state to CONNECTED state or visa versa.
    void toPublishQueue(const std::string &topic, const std::string &payload, bool retain); ///< Add a publish message to the publish queue.
    void messageReceived(std::string &&topic, std::string &&payload);                       ///< Called when a message is received from the broker.

    State state = State::DISCONNECTED; ///< The current state of the client.
    std::mutex state_mutex;            ///< Mutex to protect state changes.

    std::string host;           ///< Host of the broker.
    int port;                   ///< Port of the broker.
    std::string client_id;      ///< Client ID to use when connecting to the broker.
    int connection_timeout;     ///< Timeout in seconds for the connection to the broker.
    int connection_backoff_max; ///< Maximum time between backoff attempts in seconds.
    int keep_alive_interval;    ///< Interval in seconds between keep-alive messages.

    Client::LogLevel log_level = Client::LogLevel::NONE;                                              ///< The log level to use.
    std::function<void(Client::LogLevel, std::string)> logger = [](Client::LogLevel, std::string) {}; ///< Logger callback.

    std::string last_will_topic = "";   ///< Topic to publish the last will message to.
    std::string last_will_payload = ""; ///< Payload of the last will message.
    bool last_will_retain = false;      ///< Whether to retain the last will message.

    std::function<void(Error, std::string)> error_callback = [](Error, std::string) {}; ///< Error callback.

    Client::PublishQueueType publish_queue_type = Client::PublishQueueType::DROP; ///< The type of queue to use for the publish queue.
    size_t publish_queue_size = -1;                                               ///< Size of the publish queue.
    std::deque<std::tuple<std::string, std::string, bool>> publish_queue;         ///< Queue of publish messages to send to the broker.

    std::map<std::string, std::function<void(std::string, std::string)>> subscriptions; ///< Map of active subscriptions.

    std::unique_ptr<Connection> connection; ///< Connection to the broker.
    uint16_t packet_id = 0;                 ///< The next packet ID to use. Will overflow on 65535 to 0.
};
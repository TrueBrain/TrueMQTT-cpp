/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "TrueMQTT.h"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// This class tracks all internal variables of the client. This way the header
// doesn't need to include the internal implementation of the Client.
class TrueMQTT::Client::Impl
{
public:
    Impl(const std::string_view host,
         int port,
         const std::string_view client_id,
         std::chrono::milliseconds connection_timeout,
         std::chrono::milliseconds connection_backoff,
         std::chrono::milliseconds connection_backoff_max,
         std::chrono::milliseconds keep_alive_interval);
    ~Impl();

    class SubscriptionPart
    {
    public:
        std::map<std::string, SubscriptionPart, std::less<>> children;
        std::vector<std::function<void(std::string_view, std::string_view)>> callbacks;
    };

    void connect();                                                                                 ///< Connect to the broker.
    void disconnect();                                                                              ///< Disconnect from the broker.
    bool sendPublish(const std::string_view topic, const std::string_view message, bool retain);    ///< Send a publish message to the broker.
    bool sendSubscribe(const std::string_view topic);                                               ///< Send a subscribe message to the broker.
    bool sendUnsubscribe(const std::string_view topic);                                             ///< Send an unsubscribe message to the broker.
    void connectionStateChange(bool connected);                                                     ///< Called when a connection goes from CONNECTING state to CONNECTED state or visa versa.
    bool toPublishQueue(const std::string_view topic, const std::string_view message, bool retain); ///< Add a publish message to the publish queue.
    void messageReceived(std::string_view topic, std::string_view message);                         ///< Called when a message is received from the broker.

    void findSubscriptionMatch(std::string_view topic, std::string_view message, std::string_view topic_search, const std::map<std::string, Client::Impl::SubscriptionPart, std::less<>> &subscriptions); ///< Recursive function to find any matching subscription based on parts.

    State m_state = State::DISCONNECTED; ///< The current state of the client.
    std::mutex m_state_mutex;            ///< Mutex to protect state changes.

    std::string m_host;                                 ///< Host of the broker.
    int m_port;                                         ///< Port of the broker.
    std::string m_client_id;                            ///< Client ID to use when connecting to the broker.
    std::chrono::milliseconds m_connection_timeout;     ///< Timeout in seconds for the connection to the broker.
    std::chrono::milliseconds m_connection_backoff;     ///< Backoff time when connection to the broker failed. This is doubled every time a connection fails, up till \ref connection_backoff_max.
    std::chrono::milliseconds m_connection_backoff_max; ///< Maximum time between backoff attempts in seconds.
    std::chrono::milliseconds m_keep_alive_interval;    ///< Interval in seconds between keep-alive messages.

    Client::LogLevel m_log_level = Client::LogLevel::NONE;                                                                     ///< The log level to use.
    std::function<void(Client::LogLevel, std::string_view)> m_logger = [](Client::LogLevel, std::string_view) { /* empty */ }; ///< Logger callback.

    std::function<void(State)> m_state_change_callback = [](State) { /* empty */ }; ///< Callback when the connection state changes.

    std::string m_last_will_topic = "";   ///< Topic to publish the last will message to.
    std::string m_last_will_message = ""; ///< Message to publish on the last will topic.
    bool m_last_will_retain = false;      ///< Whether to retain the last will message.

    std::function<void(Error, std::string_view)> m_error_callback = [](Error, std::string_view) { /* empty */ }; ///< Error callback.

    Client::PublishQueueType m_publish_queue_type = Client::PublishQueueType::DROP; ///< The type of queue to use for the publish queue.
    size_t m_publish_queue_size = 0;                                                ///< Size of the publish queue.
    std::deque<std::tuple<std::string, std::string, bool>> m_publish_queue;         ///< Queue of publish messages to send to the broker.

    size_t m_send_queue_size = 1000; ///< Size of the send queue.

    std::set<std::string, std::less<>> m_subscription_topics;             ///< Flat list of topics the client is subscribed to.
    std::map<std::string, SubscriptionPart, std::less<>> m_subscriptions; ///< Tree of active subscriptions build up from the parts on the topic.

    class Connection;
    std::unique_ptr<Connection> m_connection; ///< Connection to the broker.
    uint16_t m_packet_id = 0;                 ///< The next packet ID to use. Will overflow on 65535 to 0.
};

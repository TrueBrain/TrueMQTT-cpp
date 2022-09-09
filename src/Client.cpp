/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TrueMQTT.h"
#include "Log.h"

#include <string>

using TrueMQTT::Client;

// This class tracks all internal variables of the client. This way the header
// doesn't need to include the internal implementation of the Client.
class Client::Impl
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
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
    };

    State state = State::DISCONNECTED; ///< The current state of the client.

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

    std::function<void(Error, std::string &)> error_callback = [](Error, std::string &) {}; ///< Error callback.

    Client::QueueType publish_queue_type = Client::QueueType::DROP; ///< The type of queue to use for the publish queue.
    int publish_queue_size = -1;                                    ///< Size of the publish queue.
};

Client::Client(const std::string &host, int port, const std::string &client_id, int connection_timeout, int connection_backoff_max, int keep_alive_interval)
{
    this->m_impl = std::make_unique<Client::Impl>(host, port, client_id, connection_timeout, connection_backoff_max, keep_alive_interval);

    LOG_TRACE("Constructor of client called");
}

Client::~Client()
{
    LOG_TRACE("Destructor of client called");

    this->disconnect();
}

void Client::setLogger(Client::LogLevel log_level, std::function<void(Client::LogLevel, std::string)> logger)
{
    LOG_TRACE("Setting logger to log level " + std::to_string(log_level));

    this->m_impl->log_level = log_level;
    this->m_impl->logger = logger;

    LOG_DEBUG("Log level now on " + std::to_string(this->m_impl->log_level));
}

void Client::setLastWill(const std::string &topic, const std::string &payload, bool retain)
{
    LOG_TRACE("Setting last will to topic " + topic + " with payload " + payload + " and retain " + std::to_string(retain));

    this->m_impl->last_will_topic = topic;
    this->m_impl->last_will_payload = payload;
    this->m_impl->last_will_retain = retain;
}

void Client::setErrorCallback(std::function<void(Error, std::string &)> callback)
{
    LOG_TRACE("Setting error callback");

    this->m_impl->error_callback = callback;
}

void Client::setPublishQueue(Client::QueueType queue_type, int size)
{
    LOG_TRACE("Setting publish queue to type " + std::to_string(queue_type) + " and size " + std::to_string(size));

    this->m_impl->publish_queue_type = queue_type;
    this->m_impl->publish_queue_size = size;
}

void Client::connect()
{
    if (this->m_impl->state != Client::Impl::State::DISCONNECTED)
    {
        return;
    }

    LOG_INFO("Connecting to " + this->m_impl->host + ":" + std::to_string(this->m_impl->port));

    this->m_impl->state = Client::Impl::State::CONNECTING;
}

void Client::disconnect()
{
    if (this->m_impl->state == Client::Impl::State::DISCONNECTED)
    {
        LOG_TRACE("Already disconnected");
        return;
    }

    LOG_INFO("Disconnecting from broker");

    this->m_impl->state = Client::Impl::State::DISCONNECTED;
}

void Client::publish(const std::string &topic, const std::string &payload, bool retain)
{
    LOG_DEBUG("Publishing message on topic '" + topic + "': " + payload + " (" + (retain ? "retained" : "not retained") + ")");
}

void Client::subscribe(const std::string &topic, std::function<void(std::string, std::string)> callback)
{
    LOG_DEBUG("Subscribing to topic '" + topic + "'");

    (void)callback;
}

void Client::unsubscribe(const std::string &topic)
{
    LOG_DEBUG("Unsubscribing from topic '" + topic + "'");
}

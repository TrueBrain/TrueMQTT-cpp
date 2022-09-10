/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TrueMQTT.h"
#include "Log.h"

#include <deque>
#include <map>
#include <mutex>
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
        DISCONNECTED, ///< The client is not connected to the broker, nor is it trying to connect.
        CONNECTING,   ///< The client is either connecting or reconnecting to the broker. This can be in any stage of the connection process.
        CONNECTED,    ///< The client is connected to the broker.
    };

    void sendPublish(const std::string &topic, const std::string &payload, bool retain);    ///< Send a publish message to the broker.
    void sendSubscribe(const std::string &topic);                                           ///< Send a subscribe message to the broker.
    void sendUnsubscribe(const std::string &topic);                                         ///< Send an unsubscribe message to the broker.
    void changeToConnected();                                                               ///< Called when a connection goes from CONNECTING state to CONNECTED state.
    void toPublishQueue(const std::string &topic, const std::string &payload, bool retain); ///< Add a publish message to the publish queue.

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

    std::function<void(Error, std::string &)> error_callback = [](Error, std::string &) {}; ///< Error callback.

    Client::PublishQueueType publish_queue_type = Client::PublishQueueType::DROP; ///< The type of queue to use for the publish queue.
    size_t publish_queue_size = -1;                                               ///< Size of the publish queue.
    std::deque<std::tuple<std::string, std::string, bool>> publish_queue;         ///< Queue of publish messages to send to the broker.

    std::map<std::string, std::function<void(std::string, std::string)>> subscriptions; ///< Map of active subscriptions.
};

Client::Client(const std::string &host, int port, const std::string &client_id, int connection_timeout, int connection_backoff_max, int keep_alive_interval)
{
    this->m_impl = std::make_unique<Client::Impl>(host, port, client_id, connection_timeout, connection_backoff_max, keep_alive_interval);

    LOG_TRACE(this->m_impl, "Constructor of client called");
}

Client::~Client()
{
    LOG_TRACE(this->m_impl, "Destructor of client called");

    this->disconnect();
}

void Client::setLogger(Client::LogLevel log_level, std::function<void(Client::LogLevel, std::string)> logger)
{
    LOG_TRACE(this->m_impl, "Setting logger to log level " + std::to_string(log_level));

    this->m_impl->log_level = log_level;
    this->m_impl->logger = logger;

    LOG_DEBUG(this->m_impl, "Log level now on " + std::to_string(this->m_impl->log_level));
}

void Client::setLastWill(const std::string &topic, const std::string &payload, bool retain)
{
    if (this->m_impl->state != Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(this->m_impl, "Cannot set last will when not disconnected");
        return;
    }

    LOG_TRACE(this->m_impl, "Setting last will to topic " + topic + " with payload " + payload + " and retain " + std::to_string(retain));

    this->m_impl->last_will_topic = topic;
    this->m_impl->last_will_payload = payload;
    this->m_impl->last_will_retain = retain;
}

void Client::setErrorCallback(std::function<void(Error, std::string &)> callback)
{
    LOG_TRACE(this->m_impl, "Setting error callback");

    this->m_impl->error_callback = callback;
}

void Client::setPublishQueue(Client::PublishQueueType queue_type, size_t size)
{
    if (this->m_impl->state != Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(this->m_impl, "Cannot set publish queue when not disconnected");
        return;
    }

    LOG_TRACE(this->m_impl, "Setting publish queue to type " + std::to_string(queue_type) + " and size " + std::to_string(size));

    this->m_impl->publish_queue_type = queue_type;
    this->m_impl->publish_queue_size = size;
}

void Client::connect()
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    if (this->m_impl->state != Client::Impl::State::DISCONNECTED)
    {
        return;
    }

    LOG_INFO(this->m_impl, "Connecting to " + this->m_impl->host + ":" + std::to_string(this->m_impl->port));

    this->m_impl->state = Client::Impl::State::CONNECTING;
}

void Client::disconnect()
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    if (this->m_impl->state == Client::Impl::State::DISCONNECTED)
    {
        LOG_TRACE(this->m_impl, "Already disconnected");
        return;
    }

    LOG_INFO(this->m_impl, "Disconnecting from broker");

    this->m_impl->state = Client::Impl::State::DISCONNECTED;
    this->m_impl->subscriptions.clear();
}

void Client::publish(const std::string &topic, const std::string &payload, bool retain)
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    LOG_DEBUG(this->m_impl, "Publishing message on topic '" + topic + "': " + payload + " (" + (retain ? "retained" : "not retained") + ")");

    switch (this->m_impl->state)
    {
    case Client::Impl::State::DISCONNECTED:
        LOG_ERROR(this->m_impl, "Cannot publish when disconnected");
        return;
    case Client::Impl::State::CONNECTING:
        this->m_impl->toPublishQueue(topic, payload, retain);
        return;
    case Client::Impl::State::CONNECTED:
        this->m_impl->sendPublish(topic, payload, retain);
        return;
    }
}

void Client::subscribe(const std::string &topic, std::function<void(std::string, std::string)> callback)
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    if (this->m_impl->state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(this->m_impl, "Cannot subscribe when disconnected");
        return;
    }

    LOG_DEBUG(this->m_impl, "Subscribing to topic '" + topic + "'");

    this->m_impl->subscriptions[topic] = callback;

    if (this->m_impl->state == Client::Impl::State::CONNECTED)
    {
        this->m_impl->sendSubscribe(topic);
    }
}

void Client::unsubscribe(const std::string &topic)
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    if (this->m_impl->state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(this->m_impl, "Cannot unsubscribe when disconnected");
        return;
    }

    LOG_DEBUG(this->m_impl, "Unsubscribing from topic '" + topic + "'");

    this->m_impl->subscriptions.erase(topic);

    if (this->m_impl->state == Client::Impl::State::CONNECTED)
    {
        this->m_impl->sendUnsubscribe(topic);
    }
}

void Client::Impl::sendPublish(const std::string &topic, const std::string &payload, bool retain)
{
    LOG_TRACE(this, "Sending publish message on topic '" + topic + "': " + payload + " (" + (retain ? "retained" : "not retained") + ")");
}

void Client::Impl::sendSubscribe(const std::string &topic)
{
    LOG_TRACE(this, "Sending subscribe message for topic '" + topic + "'");
}

void Client::Impl::sendUnsubscribe(const std::string &topic)
{
    LOG_TRACE(this, "Sending unsubscribe message for topic '" + topic + "'");
}

void Client::Impl::changeToConnected()
{
    std::scoped_lock lock(this->state_mutex);

    LOG_INFO(this, "Connected to broker");

    this->state = Client::Impl::State::CONNECTED;

    // Restoring subscriptions and flushing the queue is done while still under
    // the lock. This to prevent \ref disconnect from being called while we are
    // still sending messages.
    // The drawback is that we are blocking \ref publish and \ref subscribe too
    // when they are called just when we create a connection. But in the grand
    // scheme of things, this is not likely, and this makes for a far easier
    // implementation.

    // First restore any subscription.
    for (auto &subscription : this->subscriptions)
    {
        this->sendSubscribe(subscription.first);
    }
    // Flush the publish queue.
    for (auto &message : this->publish_queue)
    {
        this->sendPublish(std::get<0>(message), std::get<1>(message), std::get<2>(message));
    }
    this->publish_queue.clear();
}

void Client::Impl::toPublishQueue(const std::string &topic, const std::string &payload, bool retain)
{
    if (this->state != Client::Impl::State::CONNECTING)
    {
        LOG_ERROR(this, "Cannot queue publish message when not connecting");
        return;
    }

    switch (this->publish_queue_type)
    {
    case Client::PublishQueueType::DROP:
        LOG_WARNING(this, "Publish queue is disabled, dropping message");
        return;
    case Client::PublishQueueType::FIFO:
        if (this->publish_queue.size() >= this->publish_queue_size)
        {
            LOG_WARNING(this, "Publish queue is full, dropping oldest message on queue");
            this->publish_queue.pop_front();
        }
        break;
    case Client::PublishQueueType::LIFO:
        if (this->publish_queue.size() >= this->publish_queue_size)
        {
            LOG_WARNING(this, "Publish queue is full, dropping newest message on queue");
            this->publish_queue.pop_back();
        }
        break;
    }

    LOG_TRACE(this, "Adding message to publish queue");
    this->publish_queue.push_back({topic, payload, retain});
}

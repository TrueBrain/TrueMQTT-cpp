/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TrueMQTT.h"

#include "ClientImpl.h"
#include "Log.h"

using TrueMQTT::Client;

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

void Client::setErrorCallback(std::function<void(Error, std::string)> callback)
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
    this->m_impl->connect();
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
    this->m_impl->disconnect();
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

void Client::Impl::connectionStateChange(bool connected)
{
    std::scoped_lock lock(this->state_mutex);

    if (connected)
    {
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
    else
    {
        LOG_INFO(this, "Disconnected from broker");
        this->state = Client::Impl::State::CONNECTING;
    }
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

void Client::Impl::messageReceived(std::string &&topic, std::string &&payload)
{
    LOG_TRACE(this, "Message received on topic '" + topic + "': " + payload);

    // TODO -- Find which subscriptions match, and call the callbacks.
}

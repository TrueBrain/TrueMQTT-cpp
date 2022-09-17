/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TrueMQTT.h"

#include "ClientImpl.h"
#include "Connection.h"
#include "Log.h"

#include <sstream>

TrueMQTT::Client::Client(const std::string &host, int port, const std::string &client_id, int connection_timeout, int connection_backoff_max, int keep_alive_interval)
{
    this->m_impl = std::make_unique<Client::Impl>(host, port, client_id, connection_timeout, connection_backoff_max, keep_alive_interval);

    LOG_TRACE(this->m_impl, "Constructor of client called");
}

TrueMQTT::Client::~Client()
{
    LOG_TRACE(this->m_impl, "Destructor of client called");

    this->disconnect();
}

TrueMQTT::Client::Impl::Impl(const std::string &host, int port, const std::string &client_id, int connection_timeout, int connection_backoff_max, int keep_alive_interval)
    : host(host),
      port(port),
      client_id(client_id),
      connection_timeout(connection_timeout),
      connection_backoff_max(connection_backoff_max),
      keep_alive_interval(keep_alive_interval)
{
}

TrueMQTT::Client::Impl::~Impl()
{
}

void TrueMQTT::Client::setLogger(Client::LogLevel log_level, const std::function<void(Client::LogLevel, std::string)> &logger) const
{
    LOG_TRACE(this->m_impl, "Setting logger to log level " + std::to_string(log_level));

    this->m_impl->log_level = log_level;
    this->m_impl->logger = logger;

    LOG_DEBUG(this->m_impl, "Log level now on " + std::to_string(this->m_impl->log_level));
}

void TrueMQTT::Client::setLastWill(const std::string &topic, const std::string &payload, bool retain) const
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

void TrueMQTT::Client::setErrorCallback(const std::function<void(Error, std::string)> &callback) const
{
    LOG_TRACE(this->m_impl, "Setting error callback");

    this->m_impl->error_callback = callback;
}

void TrueMQTT::Client::setPublishQueue(Client::PublishQueueType queue_type, size_t size) const
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

void TrueMQTT::Client::connect() const
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

void TrueMQTT::Client::disconnect() const
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

void TrueMQTT::Client::publish(const std::string &topic, const std::string &payload, bool retain) const
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

void TrueMQTT::Client::subscribe(const std::string &topic, const std::function<void(std::string, std::string)> &callback) const
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    if (this->m_impl->state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(this->m_impl, "Cannot subscribe when disconnected");
        return;
    }

    LOG_DEBUG(this->m_impl, "Subscribing to topic '" + topic + "'");

    // Split the topic on /, to find each part.
    std::string part;
    std::stringstream stopic(topic);
    std::getline(stopic, part, '/');

    // Find the root node, and walk down till we find the leaf node.
    Client::Impl::SubscriptionPart *subscriptions = &this->m_impl->subscriptions.try_emplace(part).first->second;
    while (std::getline(stopic, part, '/'))
    {
        subscriptions = &subscriptions->children.try_emplace(part).first->second;
    }
    // Add the callback to the leaf node.
    subscriptions->callbacks.push_back(callback);

    this->m_impl->subscription_topics.insert(topic);
    if (this->m_impl->state == Client::Impl::State::CONNECTED)
    {
        this->m_impl->sendSubscribe(topic);
    }
}

void TrueMQTT::Client::unsubscribe(const std::string &topic) const
{
    std::scoped_lock lock(this->m_impl->state_mutex);

    if (this->m_impl->state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(this->m_impl, "Cannot unsubscribe when disconnected");
        return;
    }

    LOG_DEBUG(this->m_impl, "Unsubscribing from topic '" + topic + "'");

    // Split the topic on /, to find each part.
    std::string part;
    std::stringstream stopic(topic);
    std::getline(stopic, part, '/');

    // Find the root node, and walk down till we find the leaf node.
    std::vector<std::tuple<std::string, Client::Impl::SubscriptionPart *>> reverse;
    Client::Impl::SubscriptionPart *subscriptions = &this->m_impl->subscriptions[part];
    reverse.emplace_back(part, subscriptions);
    while (std::getline(stopic, part, '/'))
    {
        subscriptions = &subscriptions->children[part];
        reverse.emplace_back(part, subscriptions);
    }
    // Clear the callbacks in the leaf node.
    subscriptions->callbacks.clear();

    // Bookkeeping: remove any empty nodes.
    // Otherwise we will slowly grow in memory if a user does a lot of unsubscribes
    // on different topics.
    std::string remove_next = "";
    for (auto it = reverse.rbegin(); it != reverse.rend(); it++)
    {
        if (!remove_next.empty())
        {
            std::get<1>(*it)->children.erase(remove_next);
            remove_next = "";
        }

        if (std::get<1>(*it)->callbacks.empty() && std::get<1>(*it)->children.empty())
        {
            remove_next = std::get<0>(*it);
        }
    }
    if (!remove_next.empty())
    {
        this->m_impl->subscriptions.erase(remove_next);
    }

    this->m_impl->subscription_topics.erase(topic);
    if (this->m_impl->state == Client::Impl::State::CONNECTED)
    {
        this->m_impl->sendUnsubscribe(topic);
    }
}

void TrueMQTT::Client::Impl::connectionStateChange(bool connected)
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
        for (auto &subscription : this->subscription_topics)
        {
            this->sendSubscribe(subscription);
        }
        // Flush the publish queue.
        for (const auto &[topic, payload, retain] : this->publish_queue)
        {
            this->sendPublish(topic, payload, retain);
        }
        this->publish_queue.clear();
    }
    else
    {
        LOG_INFO(this, "Disconnected from broker");
        this->state = Client::Impl::State::CONNECTING;
    }
}

void TrueMQTT::Client::Impl::toPublishQueue(const std::string &topic, const std::string &payload, bool retain)
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
    this->publish_queue.emplace_back(topic, payload, retain);
}

void TrueMQTT::Client::Impl::findSubscriptionMatch(std::vector<std::function<void(std::string, std::string)>> &matching_callbacks, const std::map<std::string, Client::Impl::SubscriptionPart> &subscriptions, std::deque<std::string> &parts)
{
    // If we reached the end of the topic, do nothing anymore.
    if (parts.empty())
    {
        return;
    }

    LOG_TRACE(this, "Finding subscription match for part '" + parts.front() + "'");

    // Find the match based on the part.
    auto it = subscriptions.find(parts.front());
    if (it != subscriptions.end())
    {
        LOG_TRACE(this, "Found subscription match for part '" + parts.front() + "' with " + std::to_string(it->second.callbacks.size()) + " callbacks");

        matching_callbacks.insert(matching_callbacks.end(), it->second.callbacks.begin(), it->second.callbacks.end());

        std::deque<std::string> remaining_parts(parts.begin() + 1, parts.end());
        findSubscriptionMatch(matching_callbacks, it->second.children, remaining_parts);
    }

    // Find the match if this part is a wildcard.
    it = subscriptions.find("+");
    if (it != subscriptions.end())
    {
        LOG_TRACE(this, "Found subscription match for '+' with " + std::to_string(it->second.callbacks.size()) + " callbacks");

        matching_callbacks.insert(matching_callbacks.end(), it->second.callbacks.begin(), it->second.callbacks.end());

        std::deque<std::string> remaining_parts(parts.begin() + 1, parts.end());
        findSubscriptionMatch(matching_callbacks, it->second.children, remaining_parts);
    }

    // Find the match if the remaining is a wildcard.
    it = subscriptions.find("#");
    if (it != subscriptions.end())
    {
        LOG_TRACE(this, "Found subscription match for '#' with " + std::to_string(it->second.callbacks.size()) + " callbacks");

        matching_callbacks.insert(matching_callbacks.end(), it->second.callbacks.begin(), it->second.callbacks.end());
        // No more recursion here, as we implicit consume the rest of the parts too.
    }
}

void TrueMQTT::Client::Impl::messageReceived(std::string topic, std::string payload)
{
    LOG_TRACE(this, "Message received on topic '" + topic + "': " + payload);

    // Split the topic on the / in parts.
    std::string part;
    std::stringstream stopic(topic);
    std::deque<std::string> parts;
    while (std::getline(stopic, part, '/'))
    {
        parts.emplace_back(part);
    }

    // Find the matching subscription(s) with recursion.
    std::vector<std::function<void(std::string, std::string)>> matching_callbacks;
    findSubscriptionMatch(matching_callbacks, subscriptions, parts);

    LOG_TRACE(this, "Found " + std::to_string(matching_callbacks.size()) + " subscription(s) for topic '" + topic + "'");

    if (matching_callbacks.size() == 1)
    {
        // For a single callback there is no need to copy the topic/payload.
        matching_callbacks[0](std::move(topic), std::move(payload));
    }
    else
    {
        for (const auto &callback : matching_callbacks)
        {
            callback(topic, payload);
        }
    }
}

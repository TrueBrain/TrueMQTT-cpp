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

TrueMQTT::Client::Client(const std::string &host,
                         int port,
                         const std::string &client_id,
                         std::chrono::milliseconds connection_timeout,
                         std::chrono::milliseconds connection_backoff,
                         std::chrono::milliseconds connection_backoff_max,
                         std::chrono::milliseconds keep_alive_interval)
{
    m_impl = std::make_unique<Client::Impl>(host, port, client_id, connection_timeout, connection_backoff, connection_backoff_max, keep_alive_interval);

    LOG_TRACE(m_impl, "Constructor of client called");
}

TrueMQTT::Client::~Client()
{
    LOG_TRACE(m_impl, "Destructor of client called");

    disconnect();
}

TrueMQTT::Client::Impl::Impl(const std::string &host,
                             int port,
                             const std::string &client_id,
                             std::chrono::milliseconds connection_timeout,
                             std::chrono::milliseconds connection_backoff,
                             std::chrono::milliseconds connection_backoff_max,
                             std::chrono::milliseconds keep_alive_interval)
    : m_host(host),
      m_port(port),
      m_client_id(client_id),
      m_connection_timeout(connection_timeout),
      m_connection_backoff(connection_backoff),
      m_connection_backoff_max(connection_backoff_max),
      m_keep_alive_interval(keep_alive_interval)
{
}

TrueMQTT::Client::Impl::~Impl()
{
}

void TrueMQTT::Client::setLogger(Client::LogLevel log_level, const std::function<void(Client::LogLevel, std::string)> &logger) const
{
    LOG_TRACE(m_impl, "Setting logger to log level " + std::to_string(log_level));

    m_impl->m_log_level = log_level;
    m_impl->m_logger = logger;

    LOG_DEBUG(m_impl, "Log level now on " + std::to_string(m_impl->m_log_level));
}

void TrueMQTT::Client::setLastWill(const std::string &topic, const std::string &message, bool retain) const
{
    if (m_impl->m_state != Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot set last will when not disconnected");
        return;
    }

    LOG_TRACE(m_impl, "Setting last will to topic " + topic + " with message " + message + " and retain " + std::to_string(retain));

    m_impl->m_last_will_topic = topic;
    m_impl->m_last_will_message = message;
    m_impl->m_last_will_retain = retain;
}

void TrueMQTT::Client::setErrorCallback(const std::function<void(Error, std::string)> &callback) const
{
    LOG_TRACE(m_impl, "Setting error callback");

    m_impl->m_error_callback = callback;
}

void TrueMQTT::Client::setPublishQueue(Client::PublishQueueType queue_type, size_t size) const
{
    if (m_impl->m_state != Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot set publish queue when not disconnected");
        return;
    }

    LOG_TRACE(m_impl, "Setting publish queue to type " + std::to_string(queue_type) + " and size " + std::to_string(size));

    m_impl->m_publish_queue_type = queue_type;
    m_impl->m_publish_queue_size = size;
}

void TrueMQTT::Client::setSendQueue(size_t size) const
{
    if (m_impl->m_state != Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot set send queue when not disconnected");
        return;
    }

    LOG_TRACE(m_impl, "Setting send queue to size " + std::to_string(size));

    m_impl->m_send_queue_size = size;
}

void TrueMQTT::Client::connect() const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    if (m_impl->m_state != Client::Impl::State::DISCONNECTED)
    {
        return;
    }

    LOG_INFO(m_impl, "Connecting to " + m_impl->m_host + ":" + std::to_string(m_impl->m_port));

    m_impl->m_state = Client::Impl::State::CONNECTING;
    m_impl->connect();
}

void TrueMQTT::Client::disconnect() const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    if (m_impl->m_state == Client::Impl::State::DISCONNECTED)
    {
        LOG_TRACE(m_impl, "Already disconnected");
        return;
    }

    LOG_INFO(m_impl, "Disconnecting from broker");

    m_impl->m_state = Client::Impl::State::DISCONNECTED;
    m_impl->disconnect();
}

bool TrueMQTT::Client::publish(const std::string &topic, const std::string &message, bool retain) const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    LOG_DEBUG(m_impl, "Publishing message on topic '" + topic + "': " + message + " (" + (retain ? "retained" : "not retained") + ")");

    switch (m_impl->m_state)
    {
    case Client::Impl::State::DISCONNECTED:
        LOG_ERROR(m_impl, "Cannot publish when disconnected");
        return false;
    case Client::Impl::State::CONNECTING:
        return m_impl->toPublishQueue(topic, message, retain);
    case Client::Impl::State::CONNECTED:
        return m_impl->sendPublish(topic, message, retain);
    }

    return false;
}

void TrueMQTT::Client::subscribe(const std::string &topic, const std::function<void(std::string, std::string)> &callback) const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    if (m_impl->m_state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot subscribe when disconnected");
        return;
    }

    LOG_DEBUG(m_impl, "Subscribing to topic '" + topic + "'");

    // Split the topic on /, to find each part.
    std::string part;
    std::stringstream stopic(topic);
    std::getline(stopic, part, '/');

    // Find the root node, and walk down till we find the leaf node.
    Client::Impl::SubscriptionPart *subscriptions = &m_impl->m_subscriptions.try_emplace(part).first->second;
    while (std::getline(stopic, part, '/'))
    {
        subscriptions = &subscriptions->children.try_emplace(part).first->second;
    }
    // Add the callback to the leaf node.
    subscriptions->callbacks.push_back(callback);

    m_impl->m_subscription_topics.insert(topic);
    if (m_impl->m_state == Client::Impl::State::CONNECTED)
    {
        if (!m_impl->sendSubscribe(topic))
        {
            LOG_ERROR(m_impl, "Failed to send subscribe message. Closing connection to broker and trying again");
            m_impl->disconnect();
            m_impl->connect();
        }
    }
}

void TrueMQTT::Client::unsubscribe(const std::string &topic) const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    if (m_impl->m_state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot unsubscribe when disconnected");
        return;
    }

    LOG_DEBUG(m_impl, "Unsubscribing from topic '" + topic + "'");

    // Split the topic on /, to find each part.
    std::string part;
    std::stringstream stopic(topic);
    std::getline(stopic, part, '/');

    // Find the root node, and walk down till we find the leaf node.
    std::vector<std::tuple<std::string, Client::Impl::SubscriptionPart *>> reverse;
    Client::Impl::SubscriptionPart *subscriptions = &m_impl->m_subscriptions[part];
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
        m_impl->m_subscriptions.erase(remove_next);
    }

    m_impl->m_subscription_topics.erase(topic);
    if (m_impl->m_state == Client::Impl::State::CONNECTED)
    {
        if (!m_impl->sendUnsubscribe(topic))
        {
            LOG_ERROR(m_impl, "Failed to send subscribe message. Closing connection to broker and trying again");
            m_impl->disconnect();
            m_impl->connect();
        }
    }
}

void TrueMQTT::Client::Impl::connectionStateChange(bool connected)
{
    std::scoped_lock lock(m_state_mutex);

    if (connected)
    {
        LOG_INFO(this, "Connected to broker");

        m_state = Client::Impl::State::CONNECTED;

        // Restoring subscriptions and flushing the queue is done while still under
        // the lock. This to prevent \ref disconnect from being called while we are
        // still sending messages.
        // The drawback is that we are blocking \ref publish and \ref subscribe too
        // when they are called just when we create a connection. But in the grand
        // scheme of things, this is not likely, and this makes for a far easier
        // implementation.

        // First restore any subscription.
        for (auto &subscription : m_subscription_topics)
        {
            if (!sendSubscribe(subscription))
            {
                LOG_ERROR(this, "Failed to send subscribe message. Closing connection to broker and trying again");
                disconnect();
                connect();
                return;
            }
        }
        // Flush the publish queue.
        for (const auto &[topic, message, retain] : m_publish_queue)
        {
            if (!sendPublish(topic, message, retain))
            {
                LOG_ERROR(this, "Failed to send queued publish message. Discarding rest of queue");
                break;
            }
        }
        m_publish_queue.clear();
    }
    else
    {
        LOG_INFO(this, "Disconnected from broker");
        m_state = Client::Impl::State::CONNECTING;
    }
}

bool TrueMQTT::Client::Impl::toPublishQueue(const std::string &topic, const std::string &message, bool retain)
{
    if (m_state != Client::Impl::State::CONNECTING)
    {
        LOG_ERROR(this, "Cannot queue publish message when not connecting");
        return false;
    }

    switch (m_publish_queue_type)
    {
    case Client::PublishQueueType::DROP:
        LOG_WARNING(this, "Publish queue is disabled, dropping message");
        return false;
    case Client::PublishQueueType::FIFO:
        if (m_publish_queue.size() >= m_publish_queue_size)
        {
            LOG_WARNING(this, "Publish queue is full, dropping oldest message on queue");
            m_publish_queue.pop_front();
        }
        break;
    case Client::PublishQueueType::LIFO:
        if (m_publish_queue.size() >= m_publish_queue_size)
        {
            LOG_WARNING(this, "Publish queue is full, dropping newest message on queue");
            m_publish_queue.pop_back();
        }
        break;
    }

    LOG_TRACE(this, "Adding message to publish queue");
    m_publish_queue.emplace_back(topic, message, retain);
    return true;
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

void TrueMQTT::Client::Impl::messageReceived(std::string topic, std::string message)
{
    LOG_TRACE(this, "Message received on topic '" + topic + "': " + message);

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
    findSubscriptionMatch(matching_callbacks, m_subscriptions, parts);

    LOG_TRACE(this, "Found " + std::to_string(matching_callbacks.size()) + " subscription(s) for topic '" + topic + "'");

    if (matching_callbacks.size() == 1)
    {
        // For a single callback there is no need to copy the topic/message.
        matching_callbacks[0](std::move(topic), std::move(message));
    }
    else
    {
        for (const auto &callback : matching_callbacks)
        {
            callback(topic, message);
        }
    }
}

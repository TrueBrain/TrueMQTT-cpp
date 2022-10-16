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

TrueMQTT::Client::Client(const std::string_view host,
                         int port,
                         const std::string_view client_id,
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

TrueMQTT::Client::Impl::Impl(const std::string_view host,
                             int port,
                             const std::string_view client_id,
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

void TrueMQTT::Client::setLogger(Client::LogLevel log_level, const std::function<void(Client::LogLevel, std::string_view)> &logger) const
{
    LOG_TRACE(m_impl, "Setting logger to log level " + std::to_string(log_level));

    m_impl->m_log_level = log_level;
    m_impl->m_logger = logger;

    LOG_DEBUG(m_impl, "Log level now on " + std::to_string(m_impl->m_log_level));
}

void TrueMQTT::Client::setLastWill(const std::string_view topic, const std::string_view message, bool retain) const
{
    if (m_impl->m_state != Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot set last will when not disconnected");
        return;
    }

    LOG_TRACE(m_impl, "Setting last will to topic " + std::string(topic) + " with message " + std::string(message) + " and retain " + std::to_string(retain));

    m_impl->m_last_will_topic = topic;
    m_impl->m_last_will_message = message;
    m_impl->m_last_will_retain = retain;
}

void TrueMQTT::Client::setErrorCallback(const std::function<void(Error, std::string_view)> &callback) const
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

bool TrueMQTT::Client::publish(const std::string_view topic, const std::string_view message, bool retain) const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    LOG_DEBUG(m_impl, "Publishing message on topic '" + std::string(topic) + "': " + std::string(message) + " (" + (retain ? "retained" : "not retained") + ")");

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

void TrueMQTT::Client::subscribe(const std::string_view topic, const std::function<void(std::string_view, std::string_view)> &callback) const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    if (m_impl->m_state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot subscribe when disconnected");
        return;
    }

    LOG_DEBUG(m_impl, "Subscribing to topic '" + std::string(topic) + "'");

    // Find where in the tree the callback for this subscription should be added.
    Client::Impl::SubscriptionPart *subscriptions = nullptr;
    std::string_view topic_search = topic;
    while (true)
    {
        std::string_view part = topic_search;

        // Find the next part of the topic.
        auto pos = topic_search.find('/');
        if (pos != std::string_view::npos)
        {
            part = topic_search.substr(0, pos);
            topic_search.remove_prefix(pos + 1);
        }

        // Find the next subscription in the tree.
        if (subscriptions == nullptr)
        {
            subscriptions = &m_impl->m_subscriptions.try_emplace(std::string(part)).first->second;
        }
        else
        {
            subscriptions = &subscriptions->children.try_emplace(std::string(part)).first->second;
        }

        // If this was the last element, we're done.
        if (pos == std::string_view::npos)
        {
            break;
        }
    }

    // Add the callback to the leaf node.
    subscriptions->callbacks.push_back(callback);

    m_impl->m_subscription_topics.insert(std::string(topic));
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

void TrueMQTT::Client::unsubscribe(const std::string_view topic) const
{
    std::scoped_lock lock(m_impl->m_state_mutex);

    if (m_impl->m_state == Client::Impl::State::DISCONNECTED)
    {
        LOG_ERROR(m_impl, "Cannot unsubscribe when disconnected");
        return;
    }

    if (m_impl->m_subscription_topics.find(topic) == m_impl->m_subscription_topics.end())
    {
        LOG_ERROR(m_impl, "Cannot unsubscribe from topic '" + std::string(topic) + "' because we are not subscribed to it");
        return;
    }

    LOG_DEBUG(m_impl, "Unsubscribing from topic '" + std::string(topic) + "'");

    std::vector<std::tuple<std::string_view, Client::Impl::SubscriptionPart *>> reverse;

    // Find where in the tree the callback for this subscription should be removed.
    Client::Impl::SubscriptionPart *subscriptions = nullptr;
    std::string_view topic_search = topic;
    while (true)
    {
        std::string_view part = topic_search;

        // Find the next part of the topic.
        auto pos = topic_search.find('/');
        if (pos != std::string_view::npos)
        {
            part = topic_search.substr(0, pos);
            topic_search.remove_prefix(pos + 1);
        }

        // Find the next subscription in the tree.
        if (subscriptions == nullptr)
        {
            subscriptions = &m_impl->m_subscriptions.find(part)->second;
        }
        else
        {
            subscriptions = &subscriptions->children.find(part)->second;
        }

        // Update the reverse lookup.
        reverse.emplace_back(part, subscriptions);

        // If this was the last element, we're done.
        if (pos == std::string_view::npos)
        {
            break;
        }
    }

    // Clear the callbacks in the leaf node.
    subscriptions->callbacks.clear();

    // Bookkeeping: remove any empty nodes.
    // Otherwise we will slowly grow in memory if a user does a lot of unsubscribes
    // on different topics.
    std::string_view remove_next = "";
    for (auto it = reverse.rbegin(); it != reverse.rend(); it++)
    {
        if (!remove_next.empty())
        {
            std::get<1>(*it)->children.erase(std::get<1>(*it)->children.find(remove_next));
            remove_next = "";
        }

        if (std::get<1>(*it)->callbacks.empty() && std::get<1>(*it)->children.empty())
        {
            remove_next = std::get<0>(*it);
        }
    }
    if (!remove_next.empty())
    {
        m_impl->m_subscriptions.erase(m_impl->m_subscriptions.find(remove_next));
    }

    m_impl->m_subscription_topics.erase(m_impl->m_subscription_topics.find(topic));
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

bool TrueMQTT::Client::Impl::toPublishQueue(const std::string_view topic, const std::string_view message, bool retain)
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

void TrueMQTT::Client::Impl::findSubscriptionMatch(std::string_view topic, std::string_view message, std::string_view topic_search, const std::map<std::string, Client::Impl::SubscriptionPart, std::less<>> &subscriptions)
{
    std::string_view part = topic_search;

    // Find the next part of the topic.
    auto pos = topic_search.find('/');
    if (pos != std::string_view::npos)
    {
        part = topic_search.substr(0, pos);
        topic_search.remove_prefix(pos + 1);
    }

    // Find the match based on the part.
    auto it = subscriptions.find(part);
    if (it != subscriptions.end())
    {
        LOG_TRACE(this, "Found subscription match for part '" + std::string(part) + "' with " + std::to_string(it->second.callbacks.size()) + " callbacks");

        for (const auto &callback : it->second.callbacks)
        {
            callback(topic, message);
        }

        // Recursively find the match for the next part if we didn't reach the end.
        if (pos != std::string_view::npos)
        {
            findSubscriptionMatch(topic, message, topic_search, it->second.children);
        }
    }

    // Find the match if this part is a wildcard.
    it = subscriptions.find("+");
    if (it != subscriptions.end())
    {
        LOG_TRACE(this, "Found subscription match for '+' with " + std::to_string(it->second.callbacks.size()) + " callbacks");

        for (const auto &callback : it->second.callbacks)
        {
            callback(topic, message);
        }

        // Recursively find the match for the next part if we didn't reach the end.
        if (pos != std::string_view::npos)
        {
            findSubscriptionMatch(topic, message, topic_search, it->second.children);
        }
    }

    // Find the match if the remaining is a wildcard.
    it = subscriptions.find("#");
    if (it != subscriptions.end())
    {
        LOG_TRACE(this, "Found subscription match for '#' with " + std::to_string(it->second.callbacks.size()) + " callbacks");

        for (const auto &callback : it->second.callbacks)
        {
            callback(topic, message);
        }

        // No more recursion here, as we implicit consume the rest of the parts too.
    }
}

void TrueMQTT::Client::Impl::messageReceived(std::string_view topic, std::string_view message)
{
    std::scoped_lock lock(m_state_mutex);

    LOG_TRACE(this, "Message received on topic '" + std::string(topic) + "': " + std::string(message));

    if (m_state != State::CONNECTED)
    {
        // This happens easily when the subscribed to a really busy topic and you disconnect.
        LOG_ERROR(this, "Received message while not connected");
        return;
    }

    findSubscriptionMatch(topic, message, topic, m_subscriptions);
}

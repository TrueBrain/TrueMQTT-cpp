/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>

namespace TrueMQTT
{
    /**
     * @brief MQTT Client class.
     * This class manages the MQTT connection and provides methods to publish and subscribe to topics.
     */
    class Client
    {
    public:
        /**
         * @brief Error codes that can be returned in the callback set by \ref setErrorCallback.
         */
        enum Error
        {
            SUBSCRIBE_FAILED,   ///< The subscription failed. The topic that failed to subscribe is passed as the second argument.
            UNSUBSCRIBE_FAILED, ///< The unsubscription failed. The topic that failed to unsubscribe is passed as the second argument.
            DISCONNECTED,       ///< The connection was lost. The reason for the disconnection is passed as the second argument.
            CONNECTION_FAILED,  ///< The connection failed. The reason for the failure is passed as the second argument.
        };

        /**
         * @brief The type of queue that can be set for publishing messages.
         */
        enum QueueType
        {
            DROP,           ///< Do not queue.
            FIFO,           ///< Global FIFO.
            LIFO,           ///< Global LIFO.
            LIFO_PER_TOPIC, ///< Per topic LIFO.
        };

        /**
         * @brief The log levels used by this library.
         */
        enum LogLevel
        {
            NONE,    ///< Do not log anything (default).
            ERROR,   ///< Something went wrong and the library cannot recover.
            WARNING, ///< Something wasn't right, but the library can recover.
            INFO,    ///< Information that might be useful to know.
            DEBUG,   ///< Information that might be useful for debugging.
            TRACE,   ///< Information that is overly verbose to tell exactly what the library is doing.
        };

        /**
         * @brief Constructor for the MQTT client.
         *
         * @param host The hostname of the MQTT broker. Can be either an IP or a domain name.
         * @param port Port of the MQTT broker.
         * @param client_id Client ID to use when connecting to the broker.
         * @param connection_timeout Timeout in seconds for the connection to the broker.
         * @param connection_backoff_max Maximum time between backoff attempts in seconds.
         * @param keep_alive_interval Interval in seconds between keep-alive messages.
         */
        Client(const std::string &host, int port, const std::string &client_id, int connection_timeout = 5, int connection_backoff_max = 30, int keep_alive_interval = 30);

        /**
         * @brief Destructor of the MQTT client.
         *
         * Before destruction, any open connection is closed gracefully.
         */
        ~Client();

        /**
         * @brief Set the logger callback and level.
         *
         * @param log_level The \ref LogLevel to use for logging.
         * @param logger The callback to call when a log message is generated.
         *
         * @note This library doesn't contain a logger, so you need to provide one.
         * If this method is not called, no logging will be done.
         */
        void setLogger(LogLevel log_level, std::function<void(LogLevel, std::string)> logger);

        /**
         * @brief Set the last will message on the connection.
         *
         * @param topic The topic to publish the last will message to.
         * @param payload The payload of the last will message.
         * @param retain Whether to retain the last will message.
         */
        void setLastWill(const std::string &topic, const std::string &payload, bool retain);

        /**
         * @brief Set the error callback, called when any error occurs.
         * @param callback The callback to call when an error occurs.
         */
        void setErrorCallback(std::function<void(Error, std::string &)> callback);

        /**
         * @brief Set the publish queue to use.
         *
         * @param queue_type The \ref QueueType to use for the publish queue.
         * @param size The size of the queue. If the queue is full, the type of queue defines what happens.
         */
        void setPublishQueue(QueueType queue_type, int size);

        /**
         * @brief Connect to the broker.
         *
         * After calling this function, the library will try a connection to the broker.
         * If the connection fails, it will try again after a backoff period.
         * The backoff period will increase until it reaches the maximum backoff period.
         *
         * If the connection succeeds, but it disconnected later (without calling \ref disconnect),
         * the library will try to reconnect.
         *
         * @note Calling connect twice has no effect.
         */
        void connect();

        /**
         * @brief Disconnect from the broker.
         *
         * This function will disconnect from the broker and stop trying to reconnect.
         * Additionally, it will clean any publish / subscribe information it has.
         *
         * @note Calling disconnect twice has no effect.
         */
        void disconnect();

        /**
         * @brief Publish a payload on a topic.
         *
         * @param topic The topic to publish the payload on.
         * @param payload The payload to publish.
         * @param retain Whether to retain the message on the broker.
         *
         * @note All messages are always published under QoS 0, and this library supports no
         * other QoS level.
         * @note This call is non-blocking, and it is not possible to know whether the message
         * was actually published or not.
         */
        void publish(const std::string &topic, const std::string &payload, bool retain);

        /**
         * @brief Subscribe to a topic, and call the callback function when a message arrives.
         *
         * @param topic The topic to subscribe to.
         * @param callback The callback to call when a message arrives on this topic.
         *
         * @note Subscription can overlap, but you cannot subscribe on the exact same topic twice.
         * If you do, the callback of the first subscription will be overwritten.
         * In other words, "a/+" and "a/b" is fine, and callbacks for both subscribes will be
         * called when something is published on "a/b".
         */
        void subscribe(const std::string &topic, std::function<void(std::string, std::string)> callback);

        /**
         * @brief Unsubscribe from a topic.
         *
         * @param topic The topic to unsubscribe from.
         *
         * @note If you unsubscribe from a topic you were not subscribed too, nothing happens.
         */
        void unsubscribe(const std::string &topic);

    private:
        // Private implementation
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}

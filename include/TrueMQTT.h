/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
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
            /**
             * @brief The hostname could not be resolved into either an IPv4 or IPv6 address.
             *
             * This happens if the DNS server didn't return any valid IPv4 or IPv6 address
             * based on the hostname given.
             *
             * Due to the nature of this error, this library has no way to recover from
             * this. As such, this is considered a fatal error and the library takes no
             * attempt to gracefully handle this.
             *
             * @note This is a fatal error. You have to call \ref disconnect after this.
             */
            HOSTNAME_LOOKUP_FAILED,

            /**
             * @brief The subscription failed.
             *
             * The topic that failed to subscribe is passed as the second argument.
             *
             * @note This error is non-fatal.
             */
            SUBSCRIBE_FAILED,
        };

        /**
         * @brief The type of queue that can be set for publishing messages.
         *
         * When there is no connection to a broker, some choices have to be made what
         * to do with messages that are published.
         *
         * - Do we queue the message?
         * - What do we do when the queue is full?
         *
         * After all, memory is finite, so this allows you to configure what scenario
         * works best for you.
         */
        enum PublishQueueType
        {
            DROP, ///< Do not queue.

            /**
             * @brief First-in-First-out queue.
             *
             * For a size 3 queue this means if we publish 6 messages (M1 .. M6), the result is:
             *
             *  [ M4, M5, M6 ]
             *
             * When publishing the next message (M7) it becomes:
             *
             *  [ M5, M6, M7 ]
             */
            FIFO,

            /**
             * @brief Last-in-First-out queue.
             *
             * For a size 3 queue this means if we publish 6 messages (M1 .. M6), the result is:
             *
             *  [ M1, M2, M6 ]
             *
             * When publishing the next message (M7) it becomes:
             *
             *  [ M1, M2, M7 ]
             */
            LIFO,
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
         * @param connection_backoff Backoff time when connection to the broker failed. This is doubled every time a connection fails, up till \ref connection_backoff_max.
         * @param connection_backoff_max Maximum time between backoff attempts in seconds.
         * @param keep_alive_interval Interval in seconds between keep-alive messages.
         */
        Client(const std::string &host,
               int port,
               const std::string &client_id,
               std::chrono::milliseconds connection_timeout = std::chrono::milliseconds(5000),
               std::chrono::milliseconds connection_backoff = std::chrono::milliseconds(1000),
               std::chrono::milliseconds connection_backoff_max = std::chrono::milliseconds(30000),
               std::chrono::milliseconds keep_alive_interval = std::chrono::milliseconds(30000));

        /**
         * @brief Destructor of the MQTT client.
         *
         * Before destruction, any open connection is closed gracefully.
         */
        ~Client();

        /**
         * @brief Set the logger callback and level.
         *
         * By default no logger is set.
         *
         * @param log_level The \ref LogLevel to use for logging.
         * @param logger The callback to call when a log message is generated.
         *
         * @note This library doesn't contain a logger, so you need to provide one.
         * If this method is not called, no logging will be done.
         */
        void setLogger(LogLevel log_level, const std::function<void(LogLevel, std::string)> &logger) const;

        /**
         * @brief Set the last will message on the connection.
         *
         * By default no last will is set.
         *
         * @param topic The topic to publish the last will message to.
         * @param message The message of the last will message.
         * @param retain Whether to retain the last will message.
         *
         * @note Cannot be called after \ref connect.
         */
        void setLastWill(const std::string &topic, const std::string &message, bool retain) const;

        /**
         * @brief Set the error callback, called when any error occurs.
         *
         * @param callback The callback to call when an error occurs.
         */
        void setErrorCallback(const std::function<void(Error, std::string)> &callback) const;

        /**
         * @brief Set the publish queue to use.
         *
         * The default is DROP.
         *
         * @param queue_type The \ref PublishQueueType to use for the publish queue.
         * @param size The size of the queue. If the queue is full, the type of queue defines what happens.
         *
         * @note Cannot be called after \ref connect.
         */
        void setPublishQueue(PublishQueueType queue_type, size_t size) const;

        /**
         * @brief Set the size of the send queue.
         *
         * The send queue is used to transfer MQTT packets from the main thread to the
         * network thread. This queue is used to prevent the main thread from blocking
         * when sending a lot of data.
         *
         * Setting the queue too big will cause the memory usage to increase, while
         * setting it too small will cause functions like \ref publish to return false,
         * as the queue is full.
         *
         * The default is 1000.
         *
         * @param size Size of the send queue.
         */
        void setSendQueue(size_t size) const;

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
        void connect() const;

        /**
         * @brief Disconnect from the broker.
         *
         * This function will disconnect from the broker and stop trying to reconnect.
         * Additionally, it will clean any publish / subscribe information it has.
         *
         * @note Calling disconnect twice has no effect.
         * @note This function can stall for a short moment if you disconnect just at the
         * moment the connection to the broker is established, and there are messages in the
         * publish queue and/or subscriptions.
         */
        void disconnect() const;

        /**
         * @brief Publish a message on a topic.
         *
         * After \ref connect is called, this function will either publish the message
         * immediately (if connected) or queue it for later (if still connecting).
         * In the latter case, it will be published as soon as the connection is established.
         *
         * @param topic The topic to publish the message on.
         * @param message The message to publish.
         * @param retain Whether to retain the message on the broker.
         *
         * @return True iff the publish request is either queued or sent.
         *
         * @note All messages are always published under QoS 0, and this library supports no
         * other QoS level.
         * @note This call is non-blocking, and it is not possible to know whether the message
         * was actually published or not.
         * @note You cannot publish a message if you are disconnected from the broker. Call
         * \ref connect first.
         * @note This function can stall for a short moment if you publish just at the
         * moment the connection to the broker is established, and there are messages in the
         * publish queue and/or subscriptions.
         * @note If the return value is false, but there is a connection with the broker,
         * this means the send queue is full. It is up to the caller to consider what to do
         * in this case, but it is wise to back off for a while before sending something
         * again.
         */
        bool publish(const std::string &topic, const std::string &message, bool retain) const;

        /**
         * @brief Subscribe to a topic, and call the callback function when a message arrives.
         *
         * After \ref connect is called, this function will either subscribe to the topic
         * immediately (if connected) or subscribe to it once a connection has been made.
         * In case of a reconnect, it will also automatically resubscribe.
         *
         * If the broker refuses the subscribe request, the error-callback is called.
         *
         * @param topic The topic to subscribe to.
         * @param callback The callback to call when a message arrives on this topic.
         *
         * @note Subscription can overlap, even on the exact same topic. All callbacks that
         * match the topic will be called.
         * @note Depending on the broker, overlapping subscriptions can trigger one or more
         * calls to the same callback with the same message. This is because some brokers
         * send the message to all matching subscriptions, and some only send it to one.
         * To prevent some callbacks not being called for brokers that do the latter, this
         * library will call all matching callbacks every time.
         * Example: If you subscribe to "a/b" and "a/+", and a message arrives on "a/b",
         * both callbacks will be called. Some brokers will send a single message when
         * someone publishes to "a/b", and some will send for every subscription matching
         * (so twice in this case). In the latter case, the callback of both "a/b" and "a/+"
         * are called twice with the same "a/b" message.
         * @note You cannot subscribe a topic if you are disconnected from the broker. Call
         * \ref connect first.
         * @note This function can stall for a short moment if you publish just at the
         * moment the connection to the broker is established, and there are messages in the
         * publish queue and/or subscriptions.
         */
        void subscribe(const std::string &topic, const std::function<void(std::string, std::string)> &callback) const;

        /**
         * @brief Unsubscribe from a topic.
         *
         * @param topic The topic to unsubscribe from.
         *
         * @note If you unsubscribe from a topic you were not subscribed too, nothing happens.
         * @note This unsubscribes all subscriptions on this exact topic. It is not possible
         * to only unsubscribe from a single subscription on the same exact topic.
         * @note You cannot unsubscribe from a topic if you are disconnected from the broker.
         * Call \ref connect (and \ref subscribe) first.
         * @note This function can stall for a short moment if you publish just at the
         * moment the connection to the broker is established, and there are messages in the
         * publish queue and/or subscriptions.
         */
        void unsubscribe(const std::string &topic) const;

    private:
        // Private implementation
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}

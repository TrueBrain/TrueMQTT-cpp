/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ClientImpl.h"
#include "Packet.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <optional>
#include <string>
#include <map>
#include <mutex>
#include <netdb.h>
#include <thread>
#include <vector>

// Some definitions to make future cross-platform work easier.
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket close

class TrueMQTT::Client::Impl::Connection
{
public:
    explicit Connection(TrueMQTT::Client::Impl &impl);
    ~Connection();

    bool send(Packet packet, bool has_priority = false);
    void socketError();

private:
    // Implemented in Connection.cpp
    void runRead();
    void runWrite();
    void resolve();
    bool tryNextAddress();
    void connect(addrinfo *address);
    bool connectToAny();
    std::string addrinfoToString(const addrinfo *address) const;
    std::optional<Packet> popSendQueueBlocking();

    // Implemented in Packet.cpp
    ssize_t recv(char *buffer, size_t length);
    bool recvLoop();
    bool sendConnect();
    bool sendPingRequest();
    void sendPacket(Packet &packet);

    enum class State
    {
        RESOLVING,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        BACKOFF,
        SOCKET_ERROR,
        STOP,
    };

    TrueMQTT::Client::Impl &m_impl;

    State m_state = State::RESOLVING; ///< Current state of the connection.
    std::thread m_thread_read;        ///< Current read thread used to run this connection.
    std::thread m_thread_write;       ///< Current write thread used to run this connection.

    std::chrono::milliseconds m_backoff; ///< Current backoff time.

    addrinfo *m_host_resolved = nullptr;      ///< Address info of the hostname, once looked up.
    std::vector<addrinfo *> m_addresses = {}; ///< List of addresses to try to connect to.

    size_t m_address_current = 0;                              ///< Index of the address we are currently trying to connect to.
    std::chrono::steady_clock::time_point m_last_attempt = {}; ///< Time of the last attempt to connect to the current address.

    std::vector<SOCKET> m_sockets = {};                    ///< List of sockets we are currently trying to connect to.
    std::map<SOCKET, addrinfo *> m_socket_to_address = {}; ///< Map of sockets to the address they are trying to connect to.

    SOCKET m_socket = INVALID_SOCKET; ///< The socket we are currently connected with, or INVALID_SOCKET if not connected.

    std::deque<Packet> m_send_queue = {};    ///< Queue of packets to send to the broker.
    std::mutex m_send_queue_mutex;           ///< Mutex to protect the send queue.
    std::condition_variable m_send_queue_cv; ///< Condition variable to wake up the write thread when the send queue is not empty.

    std::chrono::steady_clock::time_point m_last_sent_packet = {};     ///< Time of the last packet sent to the broker.
    std::chrono::steady_clock::time_point m_last_received_packet = {}; ///< Time of the last packet received from the broker.
};

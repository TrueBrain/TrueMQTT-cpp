/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "TrueMQTT.h"

#include <string>
#include <map>
#include <netdb.h>
#include <thread>
#include <vector>

// Some definitions to make future cross-platform work easier.
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket close

class Connection
{
public:
    Connection(TrueMQTT::Client::LogLevel log_level,
               const std::function<void(TrueMQTT::Client::LogLevel, std::string)> logger,
               const std::function<void(TrueMQTT::Client::Error, std::string)> error_callback,
               const std::function<void(std::string &&, std::string &&)> publish_callback,
               const std::function<void(bool)> connection_change_callback,
               const std::string &host,
               int port);
    ~Connection();

    void send(class Packet &packet);

private:
    // Implemented in Connection.cpp
    void run();
    void resolve();
    bool tryNextAddress();
    void connect(addrinfo *address);
    bool connectToAny();
    std::string addrinfoToString(addrinfo *address);

    // Implemented in Packet.cpp
    ssize_t recv(char *buffer, size_t length);
    bool recvLoop();
    void sendConnect();

    enum class State
    {
        RESOLVING,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        BACKOFF,
    };

    TrueMQTT::Client::LogLevel log_level;
    const std::function<void(TrueMQTT::Client::LogLevel, std::string)> logger;

    const std::function<void(TrueMQTT::Client::Error, std::string)> m_error_callback;
    const std::function<void(std::string &&, std::string &&)> m_publish_callback;
    const std::function<void(bool)> m_connection_change_callback;

    const std::string &m_host; ///< The hostname or IP address to connect to.
    int m_port;                ///< The port to connect to.

    State m_state = State::RESOLVING;
    std::thread m_thread; ///< Current thread used to run this connection.

    addrinfo *m_host_resolved = nullptr;                       ///< Address info of the hostname, once looked up.
    std::vector<addrinfo *> m_addresses = {};                  ///< List of addresses to try to connect to.
    size_t m_address_current = 0;                              ///< Index of the address we are currently trying to connect to.
    std::chrono::steady_clock::time_point m_last_attempt = {}; ///< Time of the last attempt to connect to the current address.
    std::vector<SOCKET> m_sockets = {};                        ///< List of sockets we are currently trying to connect to.
    std::map<SOCKET, addrinfo *> m_socket_to_address = {};     ///< Map of sockets to the address they are trying to connect to.
    SOCKET m_socket = INVALID_SOCKET;                          ///< The socket we are currently connected with, or INVALID_SOCKET if not connected.
};

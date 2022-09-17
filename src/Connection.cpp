/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClientImpl.h"
#include "Connection.h"
#include "Log.h"

#include <memory.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

Connection::Connection(TrueMQTT::Client::LogLevel log_level,
                       const std::function<void(TrueMQTT::Client::LogLevel, std::string)> &logger,
                       const std::function<void(TrueMQTT::Client::Error, std::string)> &error_callback,
                       const std::function<void(std::string, std::string)> &publish_callback,
                       const std::function<void(bool)> &connection_change_callback,
                       const std::string &host,
                       int port)
    : log_level(log_level),
      logger(std::move(logger)),
      m_error_callback(std::move(error_callback)),
      m_publish_callback(std::move(publish_callback)),
      m_connection_change_callback(std::move(connection_change_callback)),
      m_host(host),
      m_port(port),
      m_thread(&Connection::run, this)
{
}

Connection::~Connection()
{
    m_state = State::STOP;

    // Make sure the connection thread is terminated.
    if (m_thread.joinable())
    {
        m_thread.join();
    }

    // freeaddrinfo() is one of those functions that doesn't take kind to NULL pointers
    // on some platforms.
    if (this->m_host_resolved != nullptr)
    {
        freeaddrinfo(this->m_host_resolved);
        this->m_host_resolved = nullptr;
    }
}

std::string Connection::addrinfoToString(const addrinfo *address) const
{
    char host[NI_MAXHOST];
    getnameinfo(address->ai_addr, address->ai_addrlen, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);

    return std::string(host);
}

void Connection::run()
{
    while (true)
    {
        switch (m_state)
        {
        case State::RESOLVING:
            resolve();
            break;

        case State::CONNECTING:
            if (!connectToAny())
            {
                m_state = State::BACKOFF;
            }
            break;

        case State::BACKOFF:
            LOG_WARNING(this, "Connection failed; will retry in NNN seconds");

            // TODO: use the configuration
            std::this_thread::sleep_for(std::chrono::seconds(5));

            m_state = State::RESOLVING;
            break;

        case State::AUTHENTICATING:
        case State::CONNECTED:
        {
            if (!recvLoop())
            {
                if (m_state == State::STOP)
                {
                    break;
                }
                if (m_socket != INVALID_SOCKET)
                {
                    closesocket(m_socket);
                    m_socket = INVALID_SOCKET;
                }
                m_state = State::BACKOFF;
                m_connection_change_callback(false);
            }
            break;
        }

        case State::STOP:
            return;
        }
    }
}

void Connection::resolve()
{
    m_address_current = 0;
    m_socket = INVALID_SOCKET;
    m_addresses.clear();

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Request IPv4 and IPv6.
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    // If we resolved previously, free the result.
    if (this->m_host_resolved != nullptr)
    {
        freeaddrinfo(this->m_host_resolved);
        this->m_host_resolved = nullptr;
    }

    // Request the OS to resolve the hostname into an IP address.
    // We do this even if the hostname is already an IP address, as that
    // makes for far easier code.
    int error = getaddrinfo(m_host.c_str(), std::to_string(m_port).c_str(), &hints, &m_host_resolved);
    if (error != 0)
    {
        m_error_callback(TrueMQTT::Client::Error::HOSTNAME_LOOKUP_FAILED, std::string(gai_strerror(error)));
        return;
    }

    // Split the list of addresses in two lists, one for IPv4 and one for
    // IPv6.
    std::deque<addrinfo *> addresses_ipv4;
    std::deque<addrinfo *> addresses_ipv6;
    for (addrinfo *ai = this->m_host_resolved; ai != nullptr; ai = ai->ai_next)
    {
        if (ai->ai_family == AF_INET6)
        {
            addresses_ipv6.emplace_back(ai);
        }
        else if (ai->ai_family == AF_INET)
        {
            addresses_ipv4.emplace_back(ai);
        }
        // Sometimes there can also be other types of families, but we are
        // not interested in those results.
    }

    // Interweave the IPv6 and IPv4 addresses. For connections we apply
    // "Happy Eyeballs", where we try an IPv6 connection first, and if that
    // doesn't connect after 100ms, we try an IPv4 connection.
    // This is to prevent long timeouts when IPv6 is not available, but
    // still prefer IPv6 where possible.
    while (!addresses_ipv6.empty() || !addresses_ipv4.empty())
    {
        if (!addresses_ipv6.empty())
        {
            m_addresses.emplace_back(addresses_ipv6.front());
            addresses_ipv6.pop_front();
        }
        if (!addresses_ipv4.empty())
        {
            m_addresses.emplace_back(addresses_ipv4.front());
            addresses_ipv4.pop_front();
        }
    }

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_DEBUG
    // For debugging, print the addresses we resolved into.
    if (this->log_level >= TrueMQTT::Client::LogLevel::DEBUG)
    {
        LOG_DEBUG(this, "Resolved hostname '" + m_host + "' to:");
        for (const addrinfo *res : m_addresses)
        {
            LOG_DEBUG(this, "- " + addrinfoToString(res));
        }
    }
#endif

    // In some odd cases, the list can be empty. This is a fatal error.
    if (m_addresses.empty())
    {
        m_error_callback(TrueMQTT::Client::Error::HOSTNAME_LOOKUP_FAILED, "");
        return;
    }

    // Only change the state if no disconnect() has been requested in the mean time.
    if (m_state != State::STOP)
    {
        m_state = State::CONNECTING;
    }
}

bool Connection::connectToAny()
{
    // Check if we have pending attempts. If not, queue a new attempt.
    if (m_sockets.empty())
    {
        return tryNextAddress();
    }

    // Check for at most 100ms if there is any activity on the sockets.
    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100;

    fd_set write_fds;
    FD_ZERO(&write_fds);
    for (const auto &socket : m_sockets)
    {
        FD_SET(socket, &write_fds);
    }

    int result = select(FD_SETSIZE, nullptr, &write_fds, nullptr, &timeout);
    // As we have waiting a bit, check if no disconnect has been requested.
    if (m_state == State::STOP)
    {
        return true;
    }

    // Check if there was an error on select(). This is hard to recover from.
    if (result < 0)
    {
        LOG_ERROR(this, "select() failed: " + std::string(strerror(errno)));
        return true;
    }

    // A result of zero means there was no activity on any of the sockets.
    if (result == 0)
    {
        // Check if it was more than 250ms ago since we started our last connection.
        if (std::chrono::steady_clock::now() < m_last_attempt + std::chrono::milliseconds(250))
        {
            return true;
        }

        // Try to queue the next address for a connection.
        if (tryNextAddress())
        {
            return true;
        }

        // Check if it is more than the timeout ago since we last tried a connection.
        // TODO -- Used to configured value
        if (std::chrono::steady_clock::now() < m_last_attempt + std::chrono::seconds(10))
        {
            return true;
        }

        LOG_ERROR(this, "Connection attempt to broker timed out");

        // Cleanup all sockets.
        for (const auto &socket : m_sockets)
        {
            closesocket(socket);
        }
        m_socket_to_address.clear();
        m_sockets.clear();

        return false;
    }

    // A socket that reports to be writeable is either connected or in error-state.
    // Remove all sockets that are in error-state. The first that is left and writeable,
    // will be the socket to use for the connection.
    SOCKET socket_connected = INVALID_SOCKET;
    for (auto socket_it = m_sockets.begin(); socket_it != m_sockets.end(); /* nothing */)
    {
        // Check if the socket is in error-state.
        int err;
        socklen_t len = sizeof(err);
        getsockopt(*socket_it, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        if (err != 0)
        {
            // It is in error-state: report about it, and remove it.
            LOG_ERROR(this, "Could not connect to " + addrinfoToString(m_socket_to_address[*socket_it]) + ": " + std::string(strerror(err)));
            closesocket(*socket_it);
            m_socket_to_address.erase(*socket_it);
            socket_it = m_sockets.erase(socket_it);
            continue;
        }

        if (socket_connected == INVALID_SOCKET && FD_ISSET(*socket_it, &write_fds))
        {
            socket_connected = *socket_it;
        }

        socket_it++;
    }

    if (socket_connected == INVALID_SOCKET)
    {
        // No socket is connected yet. Continue waiting.
        return true;
    }

    // We have a connected socket.
    LOG_DEBUG(this, "Connected to " + addrinfoToString(m_socket_to_address[socket_connected]));

    // Close all other pending connections.
    for (const auto &socket : m_sockets)
    {
        if (socket != socket_connected)
        {
            closesocket(socket);
        }
    }
    m_socket_to_address.clear();
    m_sockets.clear();

    // Disable non-blocking, as we will be reading from a thread, which can be blocking.
    int nonblocking = 0;
    if (ioctl(socket_connected, FIONBIO, &nonblocking) != 0)
    {
        LOG_WARNING(this, "Could not set socket to non-blocking; expect performance impact");
    }

    // Only change the state if no disconnect() has been requested in the mean time.
    if (m_state != State::STOP)
    {
        m_state = State::AUTHENTICATING;
        m_socket = socket_connected;
        sendConnect();
    }
    return true;
}

bool Connection::tryNextAddress()
{
    if (m_address_current >= m_addresses.size())
    {
        return false;
    }

    m_last_attempt = std::chrono::steady_clock::now();
    connect(m_addresses[m_address_current++]);

    return true;
}

void Connection::connect(addrinfo *address)
{
    // Create a new socket based on the resolved information.
    SOCKET sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (sock == INVALID_SOCKET)
    {
        LOG_ERROR(this, "Could not create new socket");
        return;
    }

    // Set socket to no-delay; this improves latency, but reduces throughput.
    int flags = 1;
    /* The (const char*) cast is needed for Windows */
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flags, sizeof(flags)) != 0)
    {
        LOG_WARNING(this, "Could not set TCP_NODELAY on socket");
    }
    // Set socket to non-blocking; this allows for multiple connects to be pending. This is
    // needed to apply Happy Eyeballs.
    int nonblocking = 1;
    if (ioctl(sock, FIONBIO, &nonblocking) != 0)
    {
        LOG_WARNING(this, "Could not set socket to non-blocking; expect performance impact");
    }

    // Start the actual connection attempt.
    LOG_DEBUG(this, "Connecting to " + addrinfoToString(address));
    int err = ::connect(sock, address->ai_addr, (int)address->ai_addrlen);
    if (err != 0 && errno != EINPROGRESS)
    {
        // As we are non-blocking, normally this returns "in progress". If anything
        // else, something is wrong. Report the error and close the socket.
        closesocket(sock);

        LOG_ERROR(this, "Could not connect to " + addrinfoToString(address) + ": " + std::string(strerror(errno)));
        return;
    }

    // Connection is pending.
    m_socket_to_address[sock] = address;
    m_sockets.push_back(sock);
}

void TrueMQTT::Client::Impl::connect()
{
    this->connection = std::make_unique<Connection>(
        this->log_level, this->logger, this->error_callback,
        [this](std::string topic, std::string payload)
        { this->messageReceived(std::move(topic), std::move(payload)); },
        [this](bool connected)
        { this->connectionStateChange(connected); },
        this->host, this->port);
}

void TrueMQTT::Client::Impl::disconnect()
{
    this->subscriptions.clear();
    this->publish_queue.clear();

    this->connection.reset();
}

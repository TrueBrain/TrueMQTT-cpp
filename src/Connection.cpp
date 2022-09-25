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

TrueMQTT::Client::Impl::Connection::Connection(Client::Impl &impl)
    : m_impl(impl),
      m_thread_read(&Connection::runRead, this),
      m_thread_write(&Connection::runWrite, this),
      m_backoff(impl.m_connection_backoff)
{
    pthread_setname_np(m_thread_read.native_handle(), "TrueMQTT::Read");
    pthread_setname_np(m_thread_write.native_handle(), "TrueMQTT::Write");
}

TrueMQTT::Client::Impl::Connection::~Connection()
{
    m_state = State::STOP;
    m_send_queue_cv.notify_one();

    // Make sure the connection thread is terminated.
    if (m_thread_read.joinable())
    {
        m_thread_read.join();
    }
    if (m_thread_write.joinable())
    {
        m_thread_write.join();
    }

    // freeaddrinfo() is one of those functions that doesn't take kind to NULL pointers
    // on some platforms.
    if (m_host_resolved != nullptr)
    {
        freeaddrinfo(m_host_resolved);
        m_host_resolved = nullptr;
    }
}

std::string TrueMQTT::Client::Impl::Connection::addrinfoToString(const addrinfo *address) const
{
    char host[NI_MAXHOST];
    getnameinfo(address->ai_addr, address->ai_addrlen, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);

    return std::string(host);
}

void TrueMQTT::Client::Impl::Connection::runRead()
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
            LOG_WARNING(&m_impl, "Connection failed; will retry in " + std::to_string(m_backoff.count()) + "ms");

            std::this_thread::sleep_for(m_backoff);

            // Calculate the next backoff time, slowly reducing how often we retry.
            m_backoff *= 2;
            if (m_backoff > m_impl.m_connection_backoff_max)
            {
                m_backoff = m_impl.m_connection_backoff_max;
            }

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
                socketError();
            }
            break;
        }

        case State::SOCKET_ERROR:
            m_state = State::BACKOFF;
            m_impl.connectionStateChange(false);

            // Clear send-queue, as we can't send anything anymore.
            {
                std::scoped_lock lock(m_send_queue_mutex);
                m_send_queue.clear();
            }
            break;

        case State::STOP:
            if (m_socket != INVALID_SOCKET)
            {
                closesocket(m_socket);
                m_socket = INVALID_SOCKET;
            }
            return;
        }
    }
}

std::optional<Packet> TrueMQTT::Client::Impl::Connection::popSendQueueBlocking()
{
    std::unique_lock<std::mutex> lock(m_send_queue_mutex);
    if (!m_send_queue.empty())
    {
        auto packet = m_send_queue.front();
        m_send_queue.pop_front();
        return packet;
    }

    m_send_queue_cv.wait(lock, [this]
                         { return !m_send_queue.empty() || m_state == State::STOP; });

    if (m_state == State::STOP)
    {
        return {};
    }

    Packet packet = m_send_queue.front();
    m_send_queue.pop_front();
    return packet;
}

void TrueMQTT::Client::Impl::Connection::runWrite()
{
    while (true)
    {
        switch (m_state)
        {
        case State::AUTHENTICATING:
        case State::CONNECTED:
        {
            auto packet = popSendQueueBlocking();
            if (!packet)
            {
                break;
            }
            sendPacket(packet.value());
            break;
        }

        case State::STOP:
            return;

        default:
            // Sleep for a bit to avoid hogging the CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            break;
        }
    }
}

void TrueMQTT::Client::Impl::Connection::socketError()
{
    m_state = State::SOCKET_ERROR;
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

void TrueMQTT::Client::Impl::Connection::resolve()
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
    if (m_host_resolved != nullptr)
    {
        freeaddrinfo(m_host_resolved);
        m_host_resolved = nullptr;
    }

    // Request the OS to resolve the hostname into an IP address.
    // We do this even if the hostname is already an IP address, as that
    // makes for far easier code.
    int error = getaddrinfo(m_impl.m_host.c_str(), std::to_string(m_impl.m_port).c_str(), &hints, &m_host_resolved);
    if (error != 0)
    {
        m_impl.m_error_callback(TrueMQTT::Client::Error::HOSTNAME_LOOKUP_FAILED, std::string(gai_strerror(error)));
        return;
    }

    // Split the list of addresses in two lists, one for IPv4 and one for
    // IPv6.
    std::deque<addrinfo *> addresses_ipv4;
    std::deque<addrinfo *> addresses_ipv6;
    for (addrinfo *ai = m_host_resolved; ai != nullptr; ai = ai->ai_next)
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
    if (m_impl.m_log_level >= TrueMQTT::Client::LogLevel::DEBUG)
    {
        LOG_DEBUG(&m_impl, "Resolved hostname '" + m_impl.m_host + "' to:");
        for (const addrinfo *res : m_addresses)
        {
            LOG_DEBUG(&m_impl, "- " + addrinfoToString(res));
        }
    }
#endif

    // In some odd cases, the list can be empty. This is a fatal error.
    if (m_addresses.empty())
    {
        m_impl.m_error_callback(TrueMQTT::Client::Error::HOSTNAME_LOOKUP_FAILED, "");
        return;
    }

    // Only change the state if no disconnect() has been requested in the mean time.
    if (m_state != State::STOP)
    {
        m_state = State::CONNECTING;
    }
}

bool TrueMQTT::Client::Impl::Connection::connectToAny()
{
    // Check if we have pending attempts. If not, queue a new attempt.
    if (m_sockets.empty())
    {
        return tryNextAddress();
    }

    // Check for at most 100ms if there is any activity on the sockets.
    timeval timeout = {0, 100 * 1000};

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
        LOG_ERROR(&m_impl, "select() failed: " + std::string(strerror(errno)));
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
        if (std::chrono::steady_clock::now() < m_last_attempt + m_impl.m_connection_timeout)
        {
            return true;
        }

        LOG_ERROR(&m_impl, "Connection attempt to broker timed out");

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
            LOG_ERROR(&m_impl, "Could not connect to " + addrinfoToString(m_socket_to_address[*socket_it]) + ": " + std::string(strerror(err)));
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
    LOG_DEBUG(&m_impl, "Connected to " + addrinfoToString(m_socket_to_address[socket_connected]));

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

    // Disable non-blocking, as we will be reading/writing from a thread, which can be blocking.
    int nonblocking = 0;
    if (ioctl(socket_connected, FIONBIO, &nonblocking) != 0)
    {
        LOG_WARNING(&m_impl, "Could not set socket to non-blocking; expect performance impact");
    }

    m_socket = socket_connected;
    m_last_sent_packet = std::chrono::steady_clock::now();
    m_last_received_packet = std::chrono::steady_clock::now();

    // Only change the state if no disconnect() has been requested in the mean time.
    if (m_state != State::STOP)
    {
        m_state = State::AUTHENTICATING;
        if (!sendConnect())
        {
            // We couldn't send the connect packet. That is unusual, so disconnect, and retry.
            LOG_ERROR(&m_impl, "Could not send first packet to broker. Disconnecting.");
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }
    }

    m_backoff = m_impl.m_connection_backoff;

    return true;
}

bool TrueMQTT::Client::Impl::Connection::tryNextAddress()
{
    if (m_address_current >= m_addresses.size())
    {
        return false;
    }

    m_last_attempt = std::chrono::steady_clock::now();
    connect(m_addresses[m_address_current++]);

    return true;
}

void TrueMQTT::Client::Impl::Connection::connect(addrinfo *address)
{
    // Create a new socket based on the resolved information.
    SOCKET sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (sock == INVALID_SOCKET)
    {
        LOG_ERROR(&m_impl, "Could not create new socket");
        return;
    }

    // Set socket to no-delay; this improves latency, but reduces throughput.
    int flags = 1;
    /* The (const char*) cast is needed for Windows */
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flags, sizeof(flags)) != 0)
    {
        LOG_WARNING(&m_impl, "Could not set TCP_NODELAY on socket");
    }
    // Set socket to non-blocking; this allows for multiple connects to be pending. This is
    // needed to apply Happy Eyeballs.
    int nonblocking = 1;
    if (ioctl(sock, FIONBIO, &nonblocking) != 0)
    {
        LOG_WARNING(&m_impl, "Could not set socket to non-blocking; expect performance impact");
    }

    // Start the actual connection attempt.
    LOG_DEBUG(&m_impl, "Connecting to " + addrinfoToString(address));
    int err = ::connect(sock, address->ai_addr, (int)address->ai_addrlen);
    if (err != 0 && errno != EINPROGRESS)
    {
        // As we are non-blocking, normally this returns "in progress". If anything
        // else, something is wrong. Report the error and close the socket.
        closesocket(sock);

        LOG_ERROR(&m_impl, "Could not connect to " + addrinfoToString(address) + ": " + std::string(strerror(errno)));
        return;
    }

    // Connection is pending.
    m_socket_to_address[sock] = address;
    m_sockets.push_back(sock);
}

void TrueMQTT::Client::Impl::connect()
{
    m_connection = std::make_unique<Connection>(*this);
}

void TrueMQTT::Client::Impl::disconnect()
{
    m_subscriptions.clear();
    m_publish_queue.clear();

    m_connection.reset();
}

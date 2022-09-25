/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClientImpl.h"
#include "Connection.h"
#include "Log.h"
#include "Packet.h"

#include "magic_enum.hpp"

#include <string.h>

ssize_t TrueMQTT::Client::Impl::Connection::recv(char *buffer, size_t length)
{
    // We idle-check every 100ms if we are requested to stop or if there was
    // an error. This is to prevent the recv() call from blocking forever.
    while (true)
    {
        // Check if there is any data available on the socket.
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(m_socket, &read_fds);
        timeval timeout = {0, 100 * 1000};
        size_t ret = select(m_socket + 1, &read_fds, nullptr, nullptr, &timeout);

        if (m_state == State::SOCKET_ERROR || m_state == State::STOP)
        {
#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_TRACE
            if (m_state == State::STOP)
            {
                LOG_TRACE(&m_impl, "Closing connection as STOP has been requested");
            }
#endif
            return -1;
        }

        auto now = std::chrono::steady_clock::now();

        // Send a keep-alive to the broker if we haven't sent anything for a while.
        if (m_last_sent_packet + m_impl.m_keep_alive_interval < now)
        {
            // Force updating the time, as it might be a while before the ping actually leaves
            // the send queue. And we don't need to send more than one for the whole interval.
            m_last_sent_packet = std::chrono::steady_clock::now();
            sendPingRequest();
        }

        // As the broker should send a ping every keep-alive interval, we really should
        // see something no later than every 1.5 times the keep-alive interval. If not,
        // we assume the connection is dead.
        if (m_last_received_packet + m_impl.m_keep_alive_interval * 15 / 10 < now)
        {
            LOG_ERROR(&m_impl, "Connection timed out");
            return -1;
        }

        if (ret == 0)
        {
            continue;
        }
        break;
    }

    ssize_t res = ::recv(m_socket, buffer, length, 0);
    if (res == 0)
    {
        LOG_WARNING(&m_impl, "Connection closed by broker");
        return -1;
    }
    if (res < 0)
    {
        LOG_WARNING(&m_impl, "Connection read error: " + std::string(strerror(errno)));
        return -1;
    }

    LOG_TRACE(&m_impl, "Received " + std::to_string(res) + " bytes");
    return res;
}

bool TrueMQTT::Client::Impl::Connection::recvLoop()
{
    uint8_t buffer;

    // Read the header of the next packet.
    if (recv((char *)&buffer, 1) <= 0)
    {
        return false;
    }

    uint8_t packet_type_raw = buffer >> 4;
    uint8_t flags = buffer & 0x0F;

    if (packet_type_raw < 1 || packet_type_raw > 14)
    {
        LOG_ERROR(&m_impl, "Received invalid packet type (" + std::to_string(packet_type_raw) + ") from broker, closing connection");
        return false;
    }

    auto packet_type = static_cast<Packet::PacketType>(packet_type_raw);

    // Read the length of the packet. This is a bit slow to read, as only
    // after reading the byte we know if another byte follows.
    // If the high-bit is set, it means there is another byte. Up to 4 bytes.
    uint32_t remaining_length = 0;
    for (int i = 0; i < 4; i++)
    {
        if (recv((char *)&buffer, 1) <= 0)
        {
            return false;
        }

        remaining_length |= (buffer & 0x7F) << (7 * i);

        if ((buffer & 0x80) == 0)
        {
            break;
        }
    }
    if ((buffer & 0x80) != 0)
    {
        LOG_ERROR(&m_impl, "Malformed packet length received, closing connection");
        return false;
    }

    LOG_TRACE(&m_impl, "Received packet of type " + std::string(magic_enum::enum_name(packet_type)) + " with flags " + std::to_string(flags) + " and length " + std::to_string(remaining_length));

    // Read the rest of the packet.
    std::vector<uint8_t> data;
    data.resize(remaining_length);

    ssize_t pos = 0;
    while (pos != remaining_length)
    {
        ssize_t res = recv((char *)&data[pos], remaining_length - pos);
        if (res <= 0)
        {
            return false;
        }
        pos += res;
    }

    Packet packet(packet_type, flags, std::move(data));

    switch (packet_type)
    {
    case Packet::PacketType::CONNACK:
    {
        uint8_t acknowledge;
        uint8_t return_code;

        if (!packet.read_uint8(acknowledge))
        {
            LOG_ERROR(&m_impl, "Malformed packet received, closing connection");
            return false;
        }
        if (!packet.read_uint8(return_code))
        {
            LOG_ERROR(&m_impl, "Malformed packet received, closing connection");
            return false;
        }

        LOG_DEBUG(&m_impl, "Received CONNACK with acknowledge " + std::to_string(acknowledge) + " and return code " + std::to_string(return_code));

        if (return_code != 0)
        {
            LOG_ERROR(&m_impl, "Broker actively refused our connection");
            return false;
        }

        m_state = State::CONNECTED;
        m_impl.connectionStateChange(true);
        break;
    }
    case Packet::PacketType::PUBLISH:
    {
        std::string topic;
        if (!packet.read_string(topic))
        {
            LOG_ERROR(&m_impl, "Malformed packet received, closing connection");
            return false;
        }

        std::string message;
        packet.read_remaining(message);

        LOG_DEBUG(&m_impl, "Received PUBLISH with topic " + topic + ": " + message);

        m_impl.messageReceived(std::move(topic), std::move(message));
        break;
    }
    case Packet::PacketType::SUBACK:
    {
        uint16_t packet_id;
        uint8_t return_code;

        if (!packet.read_uint16(packet_id))
        {
            LOG_ERROR(&m_impl, "Malformed packet received, closing connection");
            return false;
        }
        if (!packet.read_uint8(return_code))
        {
            LOG_ERROR(&m_impl, "Malformed packet received, closing connection");
            return false;
        }

        LOG_DEBUG(&m_impl, "Received SUBACK with packet id " + std::to_string(packet_id) + " and return code " + std::to_string(return_code));

        if (return_code > 2)
        {
            LOG_WARNING(&m_impl, "Broker refused our subscription");
            // TODO -- Keep track of the topic per ticket
            m_impl.m_error_callback(TrueMQTT::Client::Error::SUBSCRIBE_FAILED, "");
        }

        break;
    }
    case Packet::PacketType::UNSUBACK:
    {
        uint16_t packet_id;

        if (!packet.read_uint16(packet_id))
        {
            LOG_ERROR(&m_impl, "Malformed packet received, closing connection");
            return false;
        }

        LOG_DEBUG(&m_impl, "Received UNSUBACK with packet id " + std::to_string(packet_id));

        break;
    }
    case Packet::PacketType::PINGRESP:
    {
        LOG_DEBUG(&m_impl, "Received PINGRESP");
        break;
    }
    default:
        LOG_ERROR(&m_impl, "Received unexpected packet type " + std::string(magic_enum::enum_name(packet_type)) + " from broker, closing connection");
        return false;
    }

    m_last_received_packet = std::chrono::steady_clock::now();
    return true;
}

bool TrueMQTT::Client::Impl::Connection::send(Packet packet, bool has_priority)
{
    // Push back if the internal queue gets too big.
    if (!has_priority && m_send_queue.size() > m_impl.m_send_queue_size)
    {
        return false;
    }

    // Calculate where in the header we need to start writing, to create
    // a contiguous buffer. The buffer size is including the header, but
    // the length should be without. Hence the minus five.
    size_t length = packet.m_buffer.size() - 5;
    size_t offset = length <= 127 ? 3 : (length <= 16383 ? 2 : (length <= 2097151 ? 1 : 0));
    packet.m_write_offset = offset;

    // Set the header.
    packet.m_buffer[offset++] = (static_cast<uint8_t>(packet.m_packet_type) << 4) | packet.m_flags;

    // Set the remaining length.
    do
    {
        uint8_t byte = length & 0x7F;
        length >>= 7;
        if (length > 0)
        {
            byte |= 0x80;
        }
        packet.m_buffer[offset++] = byte;
    } while (length > 0);

    // Add the packet to the queue.
    {
        std::scoped_lock lock(m_send_queue_mutex);
        if (has_priority)
        {
            m_send_queue.push_front(std::move(packet));
        }
        else
        {
            m_send_queue.push_back(std::move(packet));
        }

    }
    // Notify the write thread that there is a new packet.
    m_send_queue_cv.notify_one();

    return true;
}

void TrueMQTT::Client::Impl::Connection::sendPacket(Packet &packet)
{
    if (m_state != State::AUTHENTICATING && m_state != State::CONNECTED)
    {
        // This happens in the small window the connection thread hasn't
        // spotted yet the connection is closed, while this function closed
        // the socket earlier due to the broker closing the connection.
        // Basically, it can only be caused if the broker actively closes
        // the connection due to a write while publishing a lot of data
        // quickly.
        LOG_DEBUG(&m_impl, "Attempted to send packet while not connected");
        return;
    }

    LOG_TRACE(&m_impl, "Sending packet of type " + std::string(magic_enum::enum_name(packet.m_packet_type)) + " with flags " + std::to_string(packet.m_flags) + " and length " + std::to_string(packet.m_buffer.size() - 5));

    // Send the packet to the broker.
    while (packet.m_write_offset < packet.m_buffer.size())
    {
        ssize_t res = ::send(m_socket, (char *)packet.m_buffer.data() + packet.m_write_offset, packet.m_buffer.size() - packet.m_write_offset, MSG_NOSIGNAL);
        if (res < 0)
        {
            LOG_ERROR(&m_impl, "Connection write error: " + std::string(strerror(errno)));
            m_impl.m_connection->socketError();
            return;
        }
        packet.m_write_offset += res;
    }

    m_last_sent_packet = std::chrono::steady_clock::now();
}

bool TrueMQTT::Client::Impl::Connection::sendConnect()
{
    LOG_TRACE(&m_impl, "Sending CONNECT packet");

    static std::string protocol_name("MQTT");

    uint8_t flags = 0;
    flags |= 1 << 1; // Clean session
    if (!m_impl.m_last_will_topic.empty())
    {
        flags |= 1 << 2; // Last will
        flags |= 0 << 3; // Last will QoS

        if (m_impl.m_last_will_retain)
        {
            flags |= 1 << 5; // Last will retain
        }
    }

    Packet packet(Packet::PacketType::CONNECT, 0);

    packet.write_string(protocol_name); // Protocol name
    packet.write_uint8(4);              // Protocol level

    packet.write_uint8(flags);
    packet.write_uint16(30); // Keep-alive

    packet.write_string(m_impl.m_client_id);
    if (!m_impl.m_last_will_topic.empty())
    {
        packet.write_string(m_impl.m_last_will_topic);
        packet.write_string(m_impl.m_last_will_message);
    }

    return send(std::move(packet), true);
}

bool TrueMQTT::Client::Impl::Connection::sendPingRequest()
{
    LOG_TRACE(&m_impl, "Sending PINGREQ packet");

    Packet packet(Packet::PacketType::PINGREQ, 0);

    return send(std::move(packet), true);
}

bool TrueMQTT::Client::Impl::sendPublish(const std::string &topic, const std::string &message, bool retain)
{
    LOG_TRACE(this, "Sending PUBLISH packet to topic '" + topic + "': " + message + " (" + (retain ? "retained" : "not retained") + ")");

    uint8_t flags = 0;
    flags |= (retain ? 1 : 0) << 0; // Retain
    flags |= 0 << 1;                // QoS
    flags |= 0 << 3;                // DUP

    Packet packet(Packet::PacketType::PUBLISH, flags);

    packet.write_string(topic);
    packet.write(message.c_str(), message.size());

    return m_connection->send(std::move(packet));
}

bool TrueMQTT::Client::Impl::sendSubscribe(const std::string &topic)
{
    LOG_TRACE(this, "Sending SUBSCRIBE packet for topic '" + topic + "'");

    Packet packet(Packet::PacketType::SUBSCRIBE, 2);

    // By specs, packet-id zero is not allowed.
    if (m_packet_id == 0)
    {
        m_packet_id++;
    }

    packet.write_uint16(m_packet_id++);
    packet.write_string(topic);
    packet.write_uint8(0); // QoS

    return m_connection->send(std::move(packet));
}

bool TrueMQTT::Client::Impl::sendUnsubscribe(const std::string &topic)
{
    LOG_TRACE(this, "Sending unsubscribe message for topic '" + topic + "'");

    Packet packet(Packet::PacketType::UNSUBSCRIBE, 2);

    // By specs, packet-id zero is not allowed.
    if (m_packet_id == 0)
    {
        m_packet_id++;
    }

    packet.write_uint16(m_packet_id++);
    packet.write_string(topic);

    return m_connection->send(std::move(packet));
}

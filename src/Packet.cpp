/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClientImpl.h"
#include "Connection.h"
#include "Log.h"

#include "magic_enum.hpp"

#include <string.h>

class Packet
{
public:
    enum class PacketType
    {
        CONNECT = 1,
        CONNACK = 2,
        PUBLISH = 3,
        PUBACK = 4,
        PUBREC = 5,
        PUBREL = 6,
        PUBCOMP = 7,
        SUBSCRIBE = 8,
        SUBACK = 9,
        UNSUBSCRIBE = 10,
        UNSUBACK = 11,
        PINGREQ = 12,
        PINGRESP = 13,
        DISCONNECT = 14,
    };

    Packet(PacketType packet_type, uint8_t flags)
        : m_packet_type(packet_type),
          m_flags(flags)
    {
    }

    Packet(PacketType packet_type, uint8_t flags, std::vector<uint8_t> data)
        : m_buffer(std::move(data)),
          m_packet_type(packet_type),
          m_flags(flags)
    {
    }

    void write_uint8(uint8_t value)
    {
        m_buffer.push_back(value);
    }

    void write_uint16(uint16_t value)
    {
        m_buffer.push_back(value >> 8);
        m_buffer.push_back(value & 0xFF);
    }

    void write(const char *data, size_t length)
    {
        m_buffer.insert(m_buffer.end(), data, data + length);
    }

    void write_string(const std::string &str)
    {
        write_uint16(str.size());
        write(str.c_str(), str.size());
    }

    bool read_uint8(uint8_t &value)
    {
        if (m_buffer.size() < m_read_offset + 1)
        {
            return false;
        }
        value = m_buffer[m_read_offset++];
        return true;
    }

    bool read_uint16(uint16_t &value)
    {
        if (m_buffer.size() < m_read_offset + 2)
        {
            return false;
        }
        value = m_buffer[m_read_offset++] << 8;
        value |= m_buffer[m_read_offset++];
        return true;
    }

    bool read_string(std::string &str)
    {
        uint16_t length;
        if (!read_uint16(length))
        {
            return false;
        }
        if (m_buffer.size() < m_read_offset + length)
        {
            return false;
        }
        const char *data = reinterpret_cast<const char *>(m_buffer.data()) + m_read_offset;
        str.assign(data, length);
        m_read_offset += length;
        return true;
    }

    void read_remaining(std::string &str)
    {
        const char *data = reinterpret_cast<const char *>(m_buffer.data()) + m_read_offset;
        str.assign(data, m_buffer.size() - m_read_offset);
        m_read_offset = m_buffer.size();
    }

    std::vector<uint8_t> m_buffer;
    size_t m_read_offset = 0;

    PacketType m_packet_type;
    uint8_t m_flags;
};

ssize_t Connection::recv(char *buffer, size_t length)
{
    ssize_t res = ::recv(m_socket, buffer, length, 0);
    if (res == 0)
    {
        LOG_INFO(this, "Connection closed by peer");
        return -1;
    }
    if (res < 0)
    {
        LOG_WARNING(this, "Connection read error: " + std::string(strerror(errno)));
        return -1;
    }

    LOG_TRACE(this, "Received " + std::to_string(res) + " bytes");
    return res;
}

bool Connection::recvLoop()
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
        LOG_ERROR(this, "Received invalid packet type (" + std::to_string(packet_type_raw) + ") from broker, closing connection");
        return false;
    }

    Packet::PacketType packet_type = static_cast<Packet::PacketType>(packet_type_raw);

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
        LOG_ERROR(this, "Malformed packet length received, closing connection");
        return false;
    }

    LOG_TRACE(this, "Received packet of type " + std::string(magic_enum::enum_name(packet_type)) + " with flags " + std::to_string(flags) + " and length " + std::to_string(remaining_length));

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
            LOG_ERROR(this, "Malformed packet received, closing connection");
            return false;
        }
        if (!packet.read_uint8(return_code))
        {
            LOG_ERROR(this, "Malformed packet received, closing connection");
            return false;
        }

        LOG_DEBUG(this, "Received CONNACK with acknowledge " + std::to_string(acknowledge) + " and return code " + std::to_string(return_code));

        if (return_code != 0)
        {
            LOG_ERROR(this, "Broker actively refused our connection");
            return false;
        }

        m_state = State::CONNECTED;
        m_connection_change_callback(true);
        break;
    }
    case Packet::PacketType::PUBLISH:
    {
        std::string topic;
        if (!packet.read_string(topic))
        {
            LOG_ERROR(this, "Malformed packet received, closing connection");
            return false;
        }

        std::string payload;
        packet.read_remaining(payload);

        LOG_DEBUG(this, "Received PUBLISH with topic " + topic + ": " + payload);

        m_publish_callback(std::move(topic), std::move(payload));
        break;
    }
    case Packet::PacketType::SUBACK:
    {
        uint16_t packet_id;
        uint8_t return_code;

        if (!packet.read_uint16(packet_id))
        {
            LOG_ERROR(this, "Malformed packet received, closing connection");
            return false;
        }
        if (!packet.read_uint8(return_code))
        {
            LOG_ERROR(this, "Malformed packet received, closing connection");
            return false;
        }

        LOG_DEBUG(this, "Received SUBACK with packet id " + std::to_string(packet_id) + " and return code " + std::to_string(return_code));

        if (return_code > 2)
        {
            LOG_WARNING(this, "Broker refused our subscription");
            // TODO -- Keep track of the topic per ticket
            m_error_callback(TrueMQTT::Client::Error::SUBSCRIBE_FAILED, "");
        }

        break;
    }
    default:
        LOG_ERROR(this, "Received unexpected packet type " + std::string(magic_enum::enum_name(packet_type)) + " from broker, closing connection");
        return false;
    }

    return true;
}

void Connection::send(Packet &packet)
{
    LOG_TRACE(this, "Sending packet of type " + std::string(magic_enum::enum_name(packet.m_packet_type)) + " with flags " + std::to_string(packet.m_flags) + " and length " + std::to_string(packet.m_buffer.size()));

    std::vector<uint8_t> buffer;

    // Create the header.
    buffer.push_back((static_cast<uint8_t>(packet.m_packet_type) << 4) | packet.m_flags);

    // Calculate the length buffer.
    size_t length = packet.m_buffer.size();
    do
    {
        uint8_t byte = length & 0x7F;
        length >>= 7;
        if (length > 0)
        {
            byte |= 0x80;
        }
        buffer.push_back(byte);
    } while (length > 0);

    // Write header and packet.
    if (::send(m_socket, (char *)buffer.data(), buffer.size(), MSG_NOSIGNAL) < 0)
    {
        LOG_WARNING(this, "Connection write error: " + std::string(strerror(errno)));
        return;
    }
    if (::send(m_socket, (char *)packet.m_buffer.data(), packet.m_buffer.size(), MSG_NOSIGNAL) < 0)
    {
        LOG_WARNING(this, "Connection write error: " + std::string(strerror(errno)));
        return;
    }
}

void Connection::sendConnect()
{
    LOG_TRACE(this, "Sending CONNECT packet");

    static std::string protocol_name("MQTT");
    static std::string client_id("ClientID");

    uint8_t flags = 0;
    flags |= 1 << 1; // Clean session
    // TODO -- Support for last-will

    Packet packet(Packet::PacketType::CONNECT, 0);

    packet.write_string(protocol_name); // Protocol name
    packet.write_uint8(4);              // Protocol level

    packet.write_uint8(flags);
    packet.write_uint16(30); // Keep-alive

    packet.write_string(client_id); // Client ID
    // TODO -- Last will topic & message

    send(packet);
}

void TrueMQTT::Client::Impl::sendPublish(const std::string &topic, const std::string &payload, bool retain)
{
    LOG_TRACE(this, "Sending PUBLISH packet to topic '" + topic + "': " + payload + " (" + (retain ? "retained" : "not retained") + ")");

    uint8_t flags = 0;
    flags |= (retain ? 1 : 0) << 0; // Retain
    flags |= 0 << 1;                // QoS
    flags |= 0 << 3;                // DUP

    Packet packet(Packet::PacketType::PUBLISH, flags);

    packet.write_string(topic);
    packet.write(payload.c_str(), payload.size());

    connection->send(packet);
}

void TrueMQTT::Client::Impl::sendSubscribe(const std::string &topic)
{
    LOG_TRACE(this, "Sending SUBSCRIBE packet for topic '" + topic + "'");

    Packet packet(Packet::PacketType::SUBSCRIBE, 2);

    // By specs, packet-id zero is not allowed.
    if (packet_id == 0)
    {
        packet_id++;
    }

    packet.write_uint16(packet_id++);
    packet.write_string(topic);
    packet.write_uint8(0); // QoS

    connection->send(packet);
}

void TrueMQTT::Client::Impl::sendUnsubscribe(const std::string &topic)
{
    LOG_TRACE(this, "Sending unsubscribe message for topic '" + topic + "'");
}

/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <vector>

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
        // Reserve space for the header.
        m_buffer.push_back(0); // Packet type and flags.
        m_buffer.push_back(0); // Remaining length (at most 4 bytes).
        m_buffer.push_back(0);
        m_buffer.push_back(0);
        m_buffer.push_back(0);
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
        write_uint16(static_cast<uint16_t>(str.size()));
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
    size_t m_write_offset = 0;

    PacketType m_packet_type;
    uint8_t m_flags;
};

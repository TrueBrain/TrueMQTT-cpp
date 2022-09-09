/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Wrappers to make logging a tiny bit easier to read.
// It heavily depends on Client.cpp's structure, and assumes
// this->m_impl is reachable.

#define LOGGER_LEVEL_ERROR 0
#define LOGGER_LEVEL_WARN 1
#define LOGGER_LEVEL_INFO 2
#define LOGGER_LEVEL_DEBUG 3
#define LOGGER_LEVEL_TRACE 4

// If no longer is defined, assume DEBUG level.
#ifndef MIN_LOGGER_LEVEL
#define MIN_LOGGER_LEVEL LOGGER_LEVEL_DEBUG
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_ERROR
#define LOG_ERROR(x)                                        \
    if (this->m_impl->log_level >= Client::LogLevel::ERROR) \
    {                                                       \
        this->m_impl->logger(Client::LogLevel::ERROR, x);   \
    }
#else
#define LOG_ERROR(x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_WARN
#define LOG_WARN(x)                                       \
    if (this->m_impl->log_level >= Client::LogLevel::WARN) \
    {                                                      \
        this->m_impl->logger(Client::LogLevel::WARN, x);   \
    }
#else
#define LOG_WARN(x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_INFO
#define LOG_INFO(x)                                       \
    if (this->m_impl->log_level >= Client::LogLevel::INFO) \
    {                                                      \
        this->m_impl->logger(Client::LogLevel::INFO, x);   \
    }
#else
#define LOG_INFO(x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_DEBUG
#define LOG_DEBUG(x)                                        \
    if (this->m_impl->log_level >= Client::LogLevel::DEBUG) \
    {                                                       \
        this->m_impl->logger(Client::LogLevel::DEBUG, x);   \
    }
#else
#define LOG_DEBUG(x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_TRACE
#define LOG_TRACE(x)                                        \
    if (this->m_impl->log_level >= Client::LogLevel::TRACE) \
    {                                                       \
        this->m_impl->logger(Client::LogLevel::TRACE, x);   \
    }
#else
#define LOG_TRACE(x)
#endif

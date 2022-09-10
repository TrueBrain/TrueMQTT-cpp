/*
 * Copyright (c) TrueBrain
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Wrappers to make logging a tiny bit easier to read.
// The "obj" is the TrueMQTT::Client::Impl instance to point to.

#define LOGGER_LEVEL_ERROR 0
#define LOGGER_LEVEL_WARNING 1
#define LOGGER_LEVEL_INFO 2
#define LOGGER_LEVEL_DEBUG 3
#define LOGGER_LEVEL_TRACE 4

// If no longer is defined, assume DEBUG level.
#ifndef MIN_LOGGER_LEVEL
#define MIN_LOGGER_LEVEL LOGGER_LEVEL_DEBUG
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_ERROR
#define LOG_ERROR(obj, x)                                    \
    if (obj->log_level >= TrueMQTT::Client::LogLevel::ERROR) \
    {                                                        \
        obj->logger(TrueMQTT::Client::LogLevel::ERROR, x);   \
    }
#else
#define LOG_ERROR(obj, x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_WARNING
#define LOG_WARNING(obj, x)                                    \
    if (obj->log_level >= TrueMQTT::Client::LogLevel::WARNING) \
    {                                                          \
        obj->logger(TrueMQTT::Client::LogLevel::WARNING, x);   \
    }
#else
#define LOG_WARNING(obj, x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_INFO
#define LOG_INFO(obj, x)                                    \
    if (obj->log_level >= TrueMQTT::Client::LogLevel::INFO) \
    {                                                       \
        obj->logger(TrueMQTT::Client::LogLevel::INFO, x);   \
    }
#else
#define LOG_INFO(obj, x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_DEBUG
#define LOG_DEBUG(obj, x)                                    \
    if (obj->log_level >= TrueMQTT::Client::LogLevel::DEBUG) \
    {                                                        \
        obj->logger(TrueMQTT::Client::LogLevel::DEBUG, x);   \
    }
#else
#define LOG_DEBUG(obj, x)
#endif

#if MIN_LOGGER_LEVEL >= LOGGER_LEVEL_TRACE
#define LOG_TRACE(obj, x)                                    \
    if (obj->log_level >= TrueMQTT::Client::LogLevel::TRACE) \
    {                                                        \
        obj->logger(TrueMQTT::Client::LogLevel::TRACE, x);   \
    }
#else
#define LOG_TRACE(obj, x)
#endif

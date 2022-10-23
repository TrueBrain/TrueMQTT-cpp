# TrueMQTT - A modern C++ MQTT Client library

This project is currently a Work In Progress.

All basic functionality is in there, but it is lacking some QoL functionalities, and it has not really been hardened / battle-tested yet.

## Development

Prepare a build folder:

```bash
mkdir build
cd build
```

Install dependencies via [conan](https://conan.io/downloads.html):

```bash
conan install ..
```

Now you can compile this library:

```bash
cmake .. -DBUILD_SHARED_LIBS=ON -DMIN_LOGGER_LEVEL=INFO
make -j$(nproc)

example/pubsub/truemqtt_pubsub
```

## Design choices

### MQTT v3 only

Although this is a contested choice, for now the library only supports MQTT v3.
There is added value in MQTT v5, but it comes with extra overhead, both in performance and memory.

This library aims to supply an interface for the more common way of using MQTT, which is a simple publish / subscribe interface.

In the future this might change, because, as said, MQTT v5 has solid additions, that might be worth delving in it.

### Copy-once

A packet that is received from a broker, is only copied once in memory (from `recv()` to an internal buffer).
All subscription callbacks get a `std::string_view` which is directly in this buffer.

This way, the library only needs to allocate memory once, heavily reducing the memory footprint.
This also means the library can handle big payloads without issue.

For publishing a similar approach is taken, and the topic / message is only copied once in an internal buffer.
The only exception here is when the client isn't connected to the broker (yet).
In this scenario, a copy of topic / message is made, and there will be two allocations for both, instead of one.

Either way, this makes this library highly efficient in terms of memory usage.

The drawback is that you have to be careful with your callbacks.
You always receive a `std::string_view`, that is only valid within that callback.
As soon as the callback returns, the memory becomes invalid.

This means that if you need to keep the topic and/or message around, you need to make a copy.

### QoS 0

This library only supports QoS 0.
This is mainly because that is the only QoS I have ever used since using MQTT.

The problem with other QoSes is that is is mostly pointless up till the point it is useful.
MQTT uses TCP, and as such, delivery over the socket is guaranteed if both sides are still alive.
In other words, QoS 1 doesn't add any guarantees in the normal situation, where lines / brokers aren't saturated.
When it does get saturated, QoS 1 becomes useful.

But, there is a trade-off here.
If you always do QoS 1 for the cases where the line does get saturated, you put more pressure on the line in all cases, which results in a line that is saturated more quickly.
And in reality, it is very hard to recover from such scenarios anyway.

MQTT 5 corrects this situation, by a bit of a cheat.
If you publish with QoS 1, but the TCP connection was working as expected, it in fact handles it as a QoS 0 request.

For this reason, this library only supports QoS 0.
As added benefit, it makes for easier code, which is less like to have bugs / problems.

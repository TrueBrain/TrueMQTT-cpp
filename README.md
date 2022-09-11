# TrueMQTT - A modern C++ MQTT Client library

This project is currently a Work In Progress, and is not functional.

## Development

```bash
mkdir build
cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DMIN_LOGGER_LEVEL=TRACE
make -j$(nproc)

example/pubsub/truemqtt_pubsub
```

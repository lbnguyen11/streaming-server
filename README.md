# Streaming Server

A real-time screen streaming server written in C++, using GStreamer for video capture and Boost.Beast for asynchronous HTTP/WebSocket streaming.

## Features

- Desktop capture using GStreamer (`ximagesrc`)
- MJPEG/WebM live stream over HTTP
- Boost.Asio & Boost.Beast async server
- Support Linux

## Build Instructions

```bash
git clone https://github.com/lbnguyen11/streaming-server.git
cd streaming-server
mkdir build && cd build
cmake ..
make

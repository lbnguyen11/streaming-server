# Streaming Server

A real-time screen streaming server written in C++, using GStreamer for video capture and Boost.Beast for asynchronous HTTP/WebSocket streaming.

## Features
- X11 desktop capture using GStreamer (`ximagesrc`)
- MJPEG/WebM live streaming over HTTP
- Boost.Asio & Boost.Beast async server
- Unit testing with doctest
- Code coverage report with gcovr
- Platform: Linux

## Dependencies

* OS: Linux
* Boost library (at least 1.71) for asynchronous HTTP/WebSocket
* Gstreamer library for screen capture
* gcovr for GCC code coverage reports

## Installing dependencies

* sudo apt update
* sudo apt install libboost-dev
* sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
* sudo apt install gcovr

## Instructions

* Clone the repo
```
git clone https://github.com/lbnguyen11/streaming-server.git
cd streaming-server
```

* Build from source
```
./01-compile.sh
```

* Run the doctest testcases
```
./02-test.sh
```

* Run the server
```
./03-run.sh
// then access http://localhost:8080/stream for MJPEG streaming and http://localhost:8080/stream01 for Webm streaming
firefox http://localhost:8080/stream
firefox http://localhost:8080/stream01
```

* Generate code coverage report
```
./04-coverage.sh
```

* All in one: build, run tests and generate code coverage report in one go
```
./00-all.sh
```

## Demo

[streaming-server.webm](https://github.com/lbnguyen11/streaming-server/blob/master/streaming-server.webm)

## Design overview

* The server spawns NUM_THREADS (currently 4) and handles asynchronous callbacks of async_accept() and async_read() concurrently among these threads (using a boost's strand object for safe handling of async_accept() calls on same acceptor)
* Kinds of streaming (MJPEG or Webm) are dispatched via target of HTTP GET requests (currently only MJPEG or Webm)
* Each kind of streaming is associated with a singleton object that implements the GStreamerIF abstract class, and multiple objects (for each http connections) that implement StreamingSessionIF abstract class
* A GStreamerIF has one gstreamer's pipeline and one lock-based std::deque of frames to be shared among http sessions of particular streaming. When the first http session connects, GStreamerIF spawns new thread to start the pipeline to push gstreamer's frames to the shared std::deque. When the last http session closes, GStreamerIF clears the shared std::deque and stop the pipeline. A StreamingSessionIF hold reference to the corresponding GStreamerIF, hence able to get frames in the shared std::deque and then send http responses to its clients.
* Each element in the shared std::deque is a std::shared_ptr of type std::array<char, CHUNK_SIZE>> (CHUNK_SIZE should be 64KB)
* To support releasing the singleton gstreamer object when the last connection is closed, the double-checked locking pattern is used to control the lifetime of the static std::unique_ptr variable
* A concrete class that implements GStreamerIF (e.g., MJPEGGstreamer) has to provide implemetation for these methods:
    * virtual size_t first_frame_to_start () const = 0: return index of first frame to be displayed
    * virtual void 	freeInstance () = 0: destroy/release the singleton object when the last connection closes
    * virtual std::shared_ptr< SharedFrame> get_frame (size_t &idx, StreamingSessionIF &) = 0: logic for getting next frame
    * virtual void 	action_when_queue_full () = 0: each kind of streaming defines its own MAX_QUEUE_SIZE and action when that limit is reached
    * virtual void 	action_if_throw () = 0: handle the exceptions thrown while reading the gstreamer's pipeline
* A concrete class that implements StreamingSessionIF (e.g., MJPEGSessionImpl) has to provide implemetation for these methods:
    * virtual void 	preupdate_when_get_frame (size_t &idx, GStreamerIF &gst): action that must be done before getting a frame from GStreamerIF's shared queue
    * virtual std::unique_ptr< http::response< http::empty_body>> create_header () const = 0: return http response's header for particular streaming's type

## Further improvements

* Use WebRTC streaming for better latency, real-time communication
* Adapt lock-free queue for better parallelism
* Extend for handling of Wayland desktop capture using pipewiresrc

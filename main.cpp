// watch -c -n 1 "ss -npi | grep -E --color=always ':8080'"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <queue>
#include <memory>
#include <cstdio>
#include <mutex>
#include <ctime>
#include <iomanip>
#include <netinet/tcp.h>
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

using namespace std;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr int PORT = 8080;
constexpr int CHUNK_SIZE = 64 * 1024;
enum
{
  GLOBAL_MAX_QUEUE_SIZE = 2 * 4096
};
using elem = std::array<char, CHUNK_SIZE>;

std::string current_time_nano()
{
  using namespace std::chrono;

  auto now = system_clock::now();
  auto now_ns = time_point_cast<nanoseconds>(now);
  auto epoch = now_ns.time_since_epoch();
  auto sec = duration_cast<seconds>(epoch);
  auto nanoseconds_part = epoch - sec;

  std::time_t tt = system_clock::to_time_t(now);
  std::tm tm = *std::localtime(&tt); // use std::gmtime for UTC time

  std::ostringstream oss;
  oss << std::put_time(&tm, "%F %T");
  oss << "." << std::setfill('0') << std::setw(9) << nanoseconds_part.count() << " ";

  return oss.str();
}

std::string get_send_buffer_info(const tcp::socket &socket)
{
  struct tcp_info info{};
  socklen_t len = sizeof(info);

  int fd = const_cast<tcp::socket &>(socket).native_handle();
  std::ostringstream oss;

  if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0)
  {
    oss << "TCP send buffer info:\n";
    oss << "  Unacked packets   : " << info.tcpi_unacked << "\n";
    oss << "  SACKs received    : " << info.tcpi_sacked << "\n";
    oss << "  Retransmits       : " << static_cast<unsigned>(info.tcpi_retransmits) << "\n";
    oss << "  Congestion window : " << info.tcpi_snd_cwnd << " packets\n"; // how many packets the sender is allowed to have in flight before needing an ACK.
    oss << "  RTT               : " << info.tcpi_rtt / 1000.0 << " ms\n";  // Round Trip Time

#if defined(__linux__) && defined(TCP_INFO_NOSENT_BYTES)
    oss << "  Not sent bytes    : " << info.tcpi_notsent_bytes << " bytes\n";
#else
    int unsent_bytes = 0;
    socklen_t len = sizeof(unsent_bytes);
    if (getsockopt(fd, SOL_TCP, TCP_INQ, &unsent_bytes, &len) == 0)
    {
      oss << "  Not sent bytes    : " << unsent_bytes << " bytes";
    }
#endif
  }
  else
  {
    oss << "getsockopt TCP_INFO failed: " << std::strerror(errno);
  }

  return oss.str();
}

// Shared frame structure
struct SharedFrame
{
  elem data;
  uint32_t sz;
  explicit SharedFrame(const elem &frame_data, size_t n)
  {
    if (n > sizeof(elem))
      throw std::runtime_error("Frame size exceeds buffer capacity");
    std::memcpy(data.data(), frame_data.data(), n);
    sz = n;
  }
};

class StreamingSession;

//////////
class GStreamerIF
{
protected:
  mutable std::mutex gst_queue_mutex_;
  mutable std::condition_variable frame_ready_;
  std::deque<std::shared_ptr<SharedFrame>> frame_queue_;
  size_t front_idx_{0};
  uint32_t max_queue_size_;
  enum
  {
    MAX_QUEUE_SIZE = 2 * 4096
  };

  FILE *pipe_{nullptr};
  std::thread pipeline_thread_;
  std::atomic<bool> stop_requested_{false};
  int session_count_{0};
  std::string name_;

public:
  GStreamerIF(uint32_t max_szie = MAX_QUEUE_SIZE, std::string name = "GStreamerIF")
      : max_queue_size_(max_szie), name_(std::move(name)) {}

  virtual ~GStreamerIF()
  {
    close_pipeline();
  }

  // virtual GStreamerIF &instance() = 0;

  void start_pipeline()
  {
    if (session_count_ == 0)
    {
      stop_requested_ = false;
      pipeline_thread_ = std::thread(&GStreamerIF::pipeline_loop, this);
      cout << current_time_nano() << "t11 pipeline_thread_ " << pipeline_thread_.get_id() << " session_count_ " << session_count_ << endl;
    }
    ++session_count_;
    cout << current_time_nano() << "t12 pipeline_thread_ " << pipeline_thread_.get_id() << " session_count_ " << session_count_ << endl;
  }

  void stop_pipeline()
  {
    {
      cout << current_time_nano() << "t13 pipeline_thread_ " << pipeline_thread_.get_id() << " session_count_ " << session_count_ << endl;
      if (--session_count_ == 0)
      {
        stop_requested_ = true;
        frame_ready_.notify_all(); // Wake up the pipeline thread
      }
      cout << current_time_nano() << "t14 pipeline_thread_ " << pipeline_thread_.get_id() << " session_count_ " << session_count_ << endl;
    }

    if (session_count_ == 0 && pipeline_thread_.joinable())
    {
      pipeline_thread_.join();
      freeInstance();
    }
  }

  void change_deuque_max_size(size_t new_max)
  {
    max_queue_size_ = new_max;
  }

  virtual std::shared_ptr<SharedFrame> get_frame(size_t &idx, StreamingSession &) = 0;

  virtual size_t first_frame_to_start() const = 0;

  size_t get_frame_count() const
  {
    std::lock_guard<std::mutex> lock(gst_queue_mutex_);
    return frame_queue_.size() + front_idx_;
  }

  size_t get_front_idx() const
  {
    std::lock_guard<std::mutex> lock(gst_queue_mutex_);
    return front_idx_;
  }

  void wait_frame_ready() const
  {
    std::unique_lock<std::mutex> lock(gst_queue_mutex_);
    frame_ready_.wait(lock);
  }

private:
  virtual void freeInstance() = 0;

  virtual FILE *get_streaming_pipe() const = 0;

protected:
  void
  pipeline_loop()
  {
    static int cnt = 0;
    try
    {
      pipe_ = get_streaming_pipe();
      if (!pipe_)
      {
        cout << current_time_nano() << "Failed to start GStreamer pipeline" << endl;
        return;
      }

      while (!stop_requested_)
      {
        // if (cnt==3000) throw std::runtime_error{"testing if pipeline_loop throws!!!"};; // for testing throw scenario
        // if (cnt==3000) std::abort(); // for testing abort scenario
        elem buf;
        uint32_t n = fread(buf.data(), 1, buf.size(), pipe_);
        if (n <= 0)
          // break;
          throw std::runtime_error{"fread n <= 0"};

        auto frame = std::make_shared<SharedFrame>(buf, n);
        cout << current_time_nano() << "t20 buf " << buf.size() << " cnt " << cnt++
             << " frame_queue_.size() " << frame_queue_.size()
             << " frame_count " << frame_queue_.size() + front_idx_
             << " front_idx_ " << front_idx_
             << " session_count_ " << session_count_
             << " stop_requested_ " << stop_requested_
             << " " << name_ << endl;
        {
          std::lock_guard<std::mutex> lock(gst_queue_mutex_);
          frame_queue_.push_back(frame);
        }
        frame_ready_.notify_all();
        action_when_queue_full();
      }

      reset_frame_queue();
      close_pipeline();
    }
    catch (const std::exception &ex)
    {
      cout << current_time_nano() << "pipeline_loop error: " << ex.what() << std::endl;
      cnt = 0;
      action_if_throw();
    }
  }

private:
  virtual void action_when_queue_full() = 0;

  virtual void action_if_throw() = 0;

  void reset_frame_queue()
  {
    std::unique_lock<std::mutex> lock(gst_queue_mutex_);
    frame_queue_.clear();
    front_idx_ = 0;
  }

protected:
  void close_pipeline()
  {
    // std::lock_guard<std::mutex> lock(gst_queue_mutex_);
    if (pipe_)
    {
      pclose(pipe_);
      pipe_ = nullptr;
    }
    // frame_queue_.clear();
  }
};

class MJPEGGStreamer : public GStreamerIF
{
private:
  static std::unique_ptr<MJPEGGStreamer> mjpeg_gst_instance_;
  static std::mutex mjpeg_gst_singleton_mutex_;
  enum
  {
    MJPEG_GST_QUEUE_SIZE = 16 * 4096
  };

public:
  static MJPEGGStreamer &getInstance()
  {
    if (!mjpeg_gst_instance_) // First check (no locking)
    {
      std::lock_guard<std::mutex> lock(mjpeg_gst_singleton_mutex_);
      if (!mjpeg_gst_instance_) // Second check (with locking)
      {
        mjpeg_gst_instance_ = std::make_unique<MJPEGGStreamer>(MJPEG_GST_QUEUE_SIZE, "MJPEGGStreamer");
      }
    }
    return *mjpeg_gst_instance_;
  }

  // GStreamerIF &instance() override
  // {
  //   return getInstance();
  // }

  MJPEGGStreamer(const MJPEGGStreamer &) = delete;
  MJPEGGStreamer &operator=(const MJPEGGStreamer &) = delete;

private:
  using GStreamerIF::GStreamerIF;
  void freeInstance() override
  {
    mjpeg_gst_instance_.reset();
  }

public:
  size_t first_frame_to_start() const override
  {
    auto cnt = get_frame_count();
    return cnt == 0 ? 0 : cnt - 1;
    // return 0; // for testing if new MJPEGSession starts streaming from first item in the queue
  }

  std::shared_ptr<SharedFrame> get_frame(size_t &idx, StreamingSession &) override
  {
    std::unique_lock<std::mutex> lock(gst_queue_mutex_);
    size_t adjustIdx = idx;
    if (idx >= front_idx_)
    {
      adjustIdx = idx - front_idx_;
    }
    else
    {
      adjustIdx = frame_queue_.size() - 1;
      idx = adjustIdx + front_idx_;
    }
    cout << current_time_nano() << "get_frame adjustIdx " << adjustIdx << endl;
    frame_ready_.wait(lock, [this, adjustIdx]
                      { return stop_requested_ || adjustIdx < frame_queue_.size(); });
    cout << current_time_nano() << "tt adjustIdx " << adjustIdx
         << " &frame_queue_[adjustIdx] " << &frame_queue_[adjustIdx]
         << " new idx " << idx << endl;
    if (stop_requested_)
    {
      return nullptr; // Return null if the pipeline is stopped
    }
    return frame_queue_[adjustIdx];
  }

private:
  FILE *get_streaming_pipe() const override
  {
    const char *cmd =
        "gst-launch-1.0 -q ximagesrc use-damage=0 ! "
        "video/x-raw,framerate=30/1 ! videoconvert ! jpegenc quality=20 ! "
        "multipartmux boundary=spionisto ! fdsink fd=1";
    return popen(cmd, "r");
  }

  void action_when_queue_full() override
  {
    std::lock_guard<std::mutex> lock(gst_queue_mutex_);
    if (frame_queue_.size() > max_queue_size_)
    {
      front_idx_++;
      frame_queue_.pop_front(); // Drop old frames if the queue is full
    }
  }

  void action_if_throw() override
  {
    close_pipeline();
    // std::this_thread::sleep_for(std::chrono::seconds(30)); // simulate server's issue
    pipeline_loop();
  }
};

// Define static MJPEGGStreamer variables
std::unique_ptr<MJPEGGStreamer> MJPEGGStreamer::mjpeg_gst_instance_;
std::mutex MJPEGGStreamer::mjpeg_gst_singleton_mutex_;

class WebmGStreamer : public GStreamerIF
{
private:
  static std::unique_ptr<WebmGStreamer> webm_gst_instance_;
  static std::mutex webm_gst_singleton_mutex_;
  enum
  {
    WEBM_GST_QUEUE_SIZE = 4 * 4096
  };

public:
  std::uint64_t pipeline_reset_cnt{0};
  static WebmGStreamer &getInstance()
  {
    if (!webm_gst_instance_) // First check (no locking)
    {
      std::lock_guard<std::mutex> lock(webm_gst_singleton_mutex_);
      if (!webm_gst_instance_) // Second check (with locking)
      {
        webm_gst_instance_ = std::make_unique<WebmGStreamer>(WEBM_GST_QUEUE_SIZE, "WebmGStreamer");
      }
    }
    return *webm_gst_instance_;
  }

  // GStreamerIF &instance() override
  // {
  //   return getInstance();
  // }

  WebmGStreamer(const WebmGStreamer &) = delete;
  WebmGStreamer &operator=(const WebmGStreamer &) = delete;

private:
  using GStreamerIF::GStreamerIF;
  void freeInstance() override
  {
    webm_gst_instance_.reset();
  }

public:
  size_t first_frame_to_start() const override
  {
    return 0;
  }

  std::shared_ptr<SharedFrame> get_frame(size_t &idx, StreamingSession &ss) override;

private:
  FILE *get_streaming_pipe() const override
  {
    const char *cmd =
        "gst-launch-1.0 -q "
        "ximagesrc use-damage=0 ! "
        "video/x-raw,framerate=120/1 ! "
        "videoconvert ! "
        "vp8enc target-bitrate=4000000 keyframe-max-dist=60 deadline=1 threads=2 ! "
        "webmmux streamable=true ! "
        "fdsink fd=1";
    return popen(cmd, "r");
  }

  void action_when_queue_full() override
  {
    std::lock_guard<std::mutex> lock(gst_queue_mutex_);
    if (frame_queue_.size() > max_queue_size_)
    {
      throw std::domain_error{"webm gst queue is full, throw to reinitialize the queue"};
    }
  }

  void action_if_throw() override
  {
    {
      std::lock_guard<std::mutex> lock(gst_queue_mutex_);
      frame_queue_.clear();
      pipeline_reset_cnt++;
    }
    close_pipeline();
    std::this_thread::sleep_for(std::chrono::seconds(1)); // simulate server's issue
    pipeline_loop();
  }
};

// Define static WebmGStreamer variables
std::unique_ptr<WebmGStreamer> WebmGStreamer::webm_gst_instance_;
std::mutex WebmGStreamer::webm_gst_singleton_mutex_;
//////////

class StreamingSession : public std::enable_shared_from_this<StreamingSession>
{
private:
  tcp::socket socket_;
  net::strand<net::io_context::executor_type> strand_;
  bool writing_{false};
  size_t frame_idx_; // Index of the current frame in the shared deque
  GStreamerIF &gst_;
  std::string name_;

public:
  explicit StreamingSession(tcp::socket &&socket, GStreamerIF &gst, std::string name = "StreamingSession")
      : socket_(std::move(socket)),
        strand_(net::make_strand(static_cast<net::io_context &>(socket_.get_executor().context()).get_executor())), // Use io_context's executor
        frame_idx_(0),
        gst_(gst),
        name_(std::move(name))
  {
  }

  virtual ~StreamingSession() = default;

  GStreamerIF &get_gst() const
  {
    return gst_;
  }

  void init_start_frame_idx(size_t idx)
  {
    frame_idx_ = idx;
  }

  virtual void preupdate_when_get_frame(size_t &idx, GStreamerIF &gst) {}

  void start()
  {
    send_header();
  }

private:
  void send_header()
  {
    auto self = this->shared_from_this();
    std::shared_ptr<http::response<http::empty_body>> res = create_header();

    http::async_write(socket_, *res, net::bind_executor(strand_, [self, res](beast::error_code ec, size_t)
                                                        {
                cout << current_time_nano() <<"t02 send_header async_write" << endl;
                if (ec)
                {
                  cout << current_time_nano() <<"t03 send_header async_write ec" << " " << ec.message() << endl;
                }
                else {
                    cout << current_time_nano() <<"t03 send_header async_write !ec" << " " << self.get() << endl;
                    self->do_write();
                } }));
  }

private:
  virtual std::unique_ptr<http::response<http::empty_body>> create_header() const = 0;

  void do_write()
  {
    auto t1 = chrono::high_resolution_clock::now();
    cout << current_time_nano() << "t05 do_write    \t\t\twriting_ " << writing_
         << " frame_idx_ " << frame_idx_
         << " frame_count " << get_gst().get_frame_count()
         << " front_idx_ " << get_gst().get_front_idx()
         << " " << this << " " << name_ << endl;
    if (writing_)
      return;

    auto frame_count = get_gst().get_frame_count();
    if (frame_idx_ >= frame_count)
    {
      get_gst().wait_frame_ready();
      // Wait for new frames to be available
      net::post(strand_, [self = this->shared_from_this()]
                { self->do_write(); });
      return;
    }

    // get the frame that relates to frame_idx_ and then update frame_idx_
    auto frame = get_gst().get_frame(frame_idx_, *this);
    auto t2 = chrono::high_resolution_clock::now();

    writing_ = true;
    auto self = this->shared_from_this();
    cout << current_time_nano() << "t06 do_write    \t\t\twriting_ " << writing_
         << " frame_count " << get_gst().get_frame_count() << " frame_idx_ " << frame_idx_ << " " << this << " " << name_ << endl;
    cout << "t06 " << name_ << " " << get_send_buffer_info(socket_) << endl;

    auto duration = chrono::duration_cast<chrono::nanoseconds>(t2 - t1);
    cout << current_time_nano() << "dd get_frame " << duration.count() << " nanosecs " << name_ << endl;
    net::async_write(socket_, net::buffer(frame->data, frame->sz),
                     net::bind_executor(strand_, [self, frame, t2](beast::error_code ec, size_t num)
                                        {
                auto t3 = chrono::high_resolution_clock::now();
                cout << "t07 " << self->name_ << " "  << self.get() << " " << get_send_buffer_info(self->socket_) << endl;
                auto duration = chrono::duration_cast<chrono::nanoseconds>(t3 - t2);
                self->writing_ = false;
                cout << current_time_nano() <<"t07 do_write async_write \twriting_ " << self->writing_
                        << " frame_count " << self->get_gst().get_frame_count()
                        << " frame_idx_ " << self->frame_idx_
                        << " ### " << num
                        << " " << self.get() << " " << self->name_ << " " << duration.count() << " nanosecs" << endl;
                self->frame_idx_ += 1; // Move to the next frame
                if (ec) {
                    cout << current_time_nano() <<"Write error: " << ec.message() << std::endl;
                    return;
                }
   
                self->do_write(); }));
  }
};

//////////----------
std::shared_ptr<SharedFrame> WebmGStreamer::get_frame(size_t &idx, StreamingSession &ss)
{
  {
    std::unique_lock<std::mutex> lock(gst_queue_mutex_);
    ss.preupdate_when_get_frame(idx, *this);
  }
  std::unique_lock<std::mutex> lock(gst_queue_mutex_);
  frame_ready_.wait(lock, [this, idx]
                    { return stop_requested_ || idx < frame_queue_.size(); });
  cout << current_time_nano() << "tt &frame_queue_[idx] " << &frame_queue_[idx]
       << " idx " << idx << endl;
  if (stop_requested_)
  {
    return nullptr; // Return null if the pipeline is stopped
  }
  return frame_queue_[idx];
}
//////////----------

class MJPEGSessionImpl : public StreamingSession
{
private:
  using base_ = StreamingSession;

public:
  explicit MJPEGSessionImpl(tcp::socket &&socket, std::string name = "MJPEGSessionImpl")
      : base_(std::move(socket), MJPEGGStreamer::getInstance(), name)
  {
    init_start_frame_idx(get_gst().first_frame_to_start());
    get_gst().start_pipeline();
  }

  ~MJPEGSessionImpl() override
  {
    get_gst().stop_pipeline();
  }

private:
  std::unique_ptr<http::response<http::empty_body>> create_header() const override
  {
    auto res = std::make_unique<http::response<http::empty_body>>(http::status::ok, 11);
    res->set(http::field::server, "AsyncMJPEGServer");
    res->set(http::field::content_type, "multipart/x-mixed-replace; boundary=spionisto");
    res->set(http::field::cache_control, "no-cache");
    res->set(http::field::connection, "close");
    res->keep_alive(false);
    return res;
  }
};

class WebmSessionImpl : public StreamingSession
{
private:
  using base_ = StreamingSession;
  std::uint64_t ss_rst_cnt{0};

public:
  explicit WebmSessionImpl(tcp::socket &&socket, std::string name = "WebmSessionImpl")
      : base_(std::move(socket), WebmGStreamer::getInstance(), name)
  {
    init_start_frame_idx(get_gst().first_frame_to_start());
    get_gst().start_pipeline();
  }

  ~WebmSessionImpl() override
  {
    get_gst().stop_pipeline();
  }

  void preupdate_when_get_frame(size_t &idx, GStreamerIF &gst) override
  {
    WebmGStreamer &webm_gst = dynamic_cast<WebmGStreamer &>(gst);
    if (ss_rst_cnt < webm_gst.pipeline_reset_cnt)
    {
      idx = 0;
      ss_rst_cnt = webm_gst.pipeline_reset_cnt;
    }
  }

private:
  std::unique_ptr<http::response<http::empty_body>> create_header() const override
  {
    auto res = std::make_unique<http::response<http::empty_body>>(http::status::ok, 11);
    res->set(http::field::server, "AsyncWEBMServer");
    res->set(http::field::content_type, "video/webm");
    res->set(http::field::cache_control, "no-cache");
    res->set(http::field::connection, "close");
    res->keep_alive(false);
    return res;
  }
};

static size_t async_accept_err_cnt = 0;

void do_accept(tcp::acceptor &acceptor, net::io_context &ioc)
{
  static size_t do_accept_cnt = 0;
  do_accept_cnt++;
  acceptor.async_accept(net::make_strand(ioc),
                        [&acceptor, &ioc](beast::error_code ec, tcp::socket socket)
                        {
                          cout << current_time_nano() << "t00 do_accept " << do_accept_cnt << " async_accept" << endl;
                          if (ec)
                          {
                            cout << current_time_nano() << "t01 do_accept " << do_accept_cnt << " async_accept ec = " << ec.message() << endl;
                            async_accept_err_cnt++;
                          }
                          else
                          {
                            cout << current_time_nano() << "t01 do_accept async_accept !ec" << endl;

                            // Start async_read to read the HTTP request
                            auto buffer = std::make_shared<beast::flat_buffer>();
                            auto req = std::make_shared<http::request<http::string_body>>();
                            auto soc = std::make_shared<tcp::socket>(std::move(socket));

                            http::async_read(*soc, *buffer, *req,
                                             [soc, buffer, req](beast::error_code ec, size_t bytes_transferred) mutable
                                             {
                                               if (ec)
                                               {
                                                 cout << current_time_nano() << "Read error: " << ec.message() << std::endl;
                                                 return;
                                               }

                                               cout << current_time_nano() << "t02 async_read: Received " << bytes_transferred << " bytes" << endl;

                                               // Check if the target is "/stream"
                                               if (req->target() == "/stream")
                                               {
                                                 cout << current_time_nano() << "t03 Target is /stream, starting MJPEGSessionImpl" << endl;
                                                 std::make_shared<MJPEGSessionImpl>(std::move(*soc))->start();
                                               }
                                               else if (req->target() == "/stream01")
                                               {
                                                 cout << current_time_nano() << "t03 Target is /stream01, starting WebmSessionImpl" << endl;
                                                 std::make_shared<WebmSessionImpl>(std::move(*soc))->start();
                                               }
                                               else
                                               {
                                                 cout << current_time_nano() << "t03 Target is not /stream, closing socket" << endl;
                                                 beast::error_code ec_close;
                                                 soc->shutdown(tcp::socket::shutdown_send, ec_close);
                                               }
                                             });
                          }

                          // Continue accepting new connections
                          do_accept(acceptor, ioc);
                        });
}

int run_my_app()
{
  try
  {
    // Set the locale to the user's default locale
    std::locale::global(std::locale(""));
    // Use the locale facet to format numbers with commas
    std::cout.imbue(std::locale("en_US.UTF-8")); // Set to US locale for comma formatting

    net::io_context ioc{1};
    tcp::acceptor acceptor{ioc, {tcp::v4(), PORT}};
    do_accept(acceptor, ioc);

    cout << current_time_nano() << "MJPEG stream at http://localhost:8080/stream" << endl;

    // Capture SIGINT and SIGTERM to perform a clean shutdown
    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&](beast::error_code const &, int)
        {
          // Stop the `io_context`. This will cause `run()`
          // to return immediately, eventually destroying the
          // `io_context` and all of the sockets in it.
          ioc.stop();
        });

    // Run the I/O service on the requested number of threads
    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> v;
    v.reserve(NUM_THREADS - 1);
    for (auto i = NUM_THREADS - 1; i > 0; --i)
    {
      v.emplace_back(
          [&ioc]
          {
            sleep(1);
            cout << current_time_nano() << "New worker thread " << std::this_thread::get_id() << endl;
            ioc.run();
          });
    }
    cout << current_time_nano() << "Starting worker threads..." << endl;
    sleep(2);
    cout << current_time_nano() << "Done! Run ioc.run() on main thread " << std::this_thread::get_id() << endl;
    ioc.run();

    // (If we get here, it means we got a SIGINT or SIGTERM)

    // Block until all the threads exit
    for (auto &t : v)
      t.join();

    return EXIT_SUCCESS;
  }
  catch (const std::exception &ex)
  {
    cout << current_time_nano() << "Fatal error: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}

int main(int argc, char **argv)
{
  doctest::Context context;

  // !!! THIS IS JUST AN EXAMPLE SHOWING HOW DEFAULTS/OVERRIDES ARE SET !!!

  // defaults
  context.addFilter("test-case-exclude", "*math*"); // exclude test cases with "math" in their name
  context.setOption("abort-after", 5);              // stop test execution after 5 failed assertions
  context.setOption("order-by", "name");            // sort the test cases by their name

  context.applyCommandLine(argc, argv);

  // overrides
  context.setOption("no-breaks", true); // don't break in the debugger when assertions fail

  int res = context.run(); // run

  if (context.shouldExit()) // important - query flags (and --exit) rely on the user doing this
    return res;             // propagate the result of the tests

  int client_stuff_return_code = 0;
  // your program - if the testing framework is integrated in your production code
  client_stuff_return_code = run_my_app();

  return res + client_stuff_return_code; // the result from doctest is propagated here as well
}

//////////TEST_CASE
TEST_CASE("001 current_time_nano() -> returns non-empty string")
{
  CHECK(current_time_nano().size() > 0);
}

TEST_CASE("002 get_send_buffer_info(socket) with invalid socket -> returns error string")
{
  net::io_context test_ioc;
  tcp::socket test_invalid_socket(test_ioc);
  CHECK(get_send_buffer_info(test_invalid_socket).substr(0, 27) == "getsockopt TCP_INFO failed:");
}

// TEST_CASE("003 get_send_buffer_info(socket) with valid socket -> returns valid data string")
// {
//   boost::asio::io_context io_context;

//   // Resolve address and port
//   tcp::resolver resolver(io_context);
//   auto endpoints = resolver.resolve("example.com", "80");

//   // Create socket and connect
//   tcp::socket socket(io_context);
//   boost::asio::connect(socket, endpoints);
//   CHECK(get_send_buffer_info(socket).substr(0, 21) == "TCP send buffer info:");
// }

TEST_CASE("004 SharedFrame(frame_data, n) with invalid n -> throws std::runtime_error")
{
  elem frame_data;
  size_t n = sizeof(frame_data) + 1;
  CHECK_THROWS_AS(SharedFrame(frame_data, n), std::runtime_error);
}

TEST_CASE("005 SharedFrame(frame_data, n) with valid n -> does memcpy exactly n bytes")
{
  elem frame_data;
  size_t n = sizeof(frame_data) / 4096;
  SharedFrame sf{frame_data, n};
  CHECK(sf.sz == n);
  for (int i = 0; i < n; ++i)
  {
    CHECK(sf.data[i] == frame_data[i]);
  }
}

TEST_CASE("006 do_accept(acceptor, &ioc) -> increases async_accept_err_cnt incase async_accept's error")
{
  CHECK(async_accept_err_cnt == 0);
  auto server_ptr = std::make_shared<net::io_context>();
  tcp::acceptor acceptor{*server_ptr}; // not valid to be use in async_acept, since it missings ip:port
  do_accept(acceptor, *server_ptr);

  auto fut = async(std::launch::async, [server_ptr]
                   { server_ptr->stop(); });

  server_ptr->run();
  fut.get();

  CHECK(async_accept_err_cnt > 0);
}

TEST_CASE("007 do_accept(acceptor, &ioc) -> calls async_accept and then calls do_accept again")
{
  std::cout.setstate(std::ios_base::failbit);
  auto server_ptr = std::make_shared<net::io_context>();
  tcp::acceptor acceptor{*server_ptr, {tcp::v4(), PORT}};
  do_accept(acceptor, *server_ptr);

  auto start_client = [](std::shared_ptr<net::io_context> server,
                         std::function<bool()> do_req,
                         std::string req_target)
  {
    struct final_action
    {
      std::function<void()> act;
      final_action(std::function<void()> a) : act{a} {}
      ~final_action() { act(); }
    };

    // stop the server before out of this function
    auto finally = final_action([&server]()
                                {
      sleep(1);
      server->stop(); });

    // The io_context is required for all I/O
    net::io_context ioc;

    // These objects perform our I/O
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    // Look up the domain name
    auto const host = "0.0.0.0";
    auto const results = resolver.resolve(host, std::to_string(PORT));

    // Make the connection on the IP address we get from a lookup
    stream.connect(results);

    if (!do_req())
      return;

    // Set up an HTTP GET request message
    auto const target = std::move(req_target);
    int version = 11;
    http::request<http::string_body> req{http::verb::get, target, version};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "test-agent");

    // Send the HTTP request to the remote host
    http::write(stream, req);
  };

  auto do_async_and_run_ioc_and_get_future =
      [start_client, server_ptr](std::function<bool()> func, std::string req_target = "")
  {
    auto fut = async(std::launch::async, start_client, server_ptr, func, std::move(req_target));
    server_ptr->run();
    fut.get();
  };

  SUBCASE("007.001 client does connect_then_exit")
  {
    auto connect_then_exit = []
    { return false; };
    do_async_and_run_ioc_and_get_future(connect_then_exit);
  }

  SUBCASE("007.002 client does connect_then_send_req 'random-str'")
  {
    auto connect_then_send_req = []
    { return true; };
    do_async_and_run_ioc_and_get_future(connect_then_send_req, "random-str");
  }

  SUBCASE("007.003 client does connect_then_send_req '/stream' for MJPEG streaming")
  {
    auto connect_then_send_req = []
    { return true; };
    do_async_and_run_ioc_and_get_future(connect_then_send_req, "/stream");
  }

  SUBCASE("007.004 client does connect_then_send_req '/stream001' for Webm streaming")
  {
    auto connect_then_send_req = []
    { return true; };
    do_async_and_run_ioc_and_get_future(connect_then_send_req, "/stream01");
  }

  std::cout.clear();
}

TEST_CASE("008 MJPEGGStreamer::first_frame_to_start() -> is not 0 when queue is non-empty ")
{
  std::cout.setstate(std::ios_base::failbit);
  cout << "tc1" << endl;
  net::io_context test_ioc;
  tcp::socket test_socket(test_ioc);
  MJPEGSessionImpl test_mjpeg_ss(std::move(test_socket));
  // MJPEGGStreamer::getInstance().start_pipeline();
  MJPEGGStreamer::getInstance().change_deuque_max_size(4);
  cout << "tc0" << endl;
  sleep(1);
  cout << "tc0a" << endl;
  cout << MJPEGGStreamer::getInstance().get_frame_count() << endl;
  cout << "tc0b" << endl;
  size_t test_frame_idx = 2;
  MJPEGGStreamer::getInstance().get_frame(test_frame_idx, test_mjpeg_ss);
  CHECK(MJPEGGStreamer::getInstance().first_frame_to_start() > 0);
  // MJPEGGStreamer::getInstance().stop_pipeline();
  cout << "tc2" << endl;
  std::cout.clear();
}

TEST_CASE("009 WebmGStreamer::first_frame_to_start() -> is always 0")
{
  std::cout.setstate(std::ios_base::failbit);
  cout << "tc1" << endl;
  net::io_context test_ioc;
  tcp::socket test_socket(test_ioc);
  WebmSessionImpl test_webm_ss(std::move(test_socket));
  // WebmGStreamer::getInstance().start_pipeline();
  WebmGStreamer::getInstance().change_deuque_max_size(1);
  sleep(2);
  cout << WebmGStreamer::getInstance().get_frame_count() << endl;
  size_t test_frame_idx = 0;
  WebmGStreamer::getInstance().get_frame(test_frame_idx, test_webm_ss);
  CHECK(WebmGStreamer::getInstance().first_frame_to_start() == 0);
  // WebmGStreamer::getInstance().stop_pipeline();
  cout << "tc2" << endl;
  std::cout.clear();
}
//////////TEST_CASE

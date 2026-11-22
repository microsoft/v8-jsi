// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#include "inspector_tcp.h"

#include <memory>
#include <algorithm>
#include <map>
#include "V8Windows.h"
#include <asio.hpp>

namespace inspector {

std::shared_ptr<ConnectionAccount> TransportBudget::TryAdmitConnection() {
  // Allocate before taking the leaf lock; allocation failure can destroy an account.
  auto account = std::shared_ptr<ConnectionAccount>(new ConnectionAccount(shared_from_this()));
  std::lock_guard<std::mutex> lock(mutex_);
  if (stats_.admitted == limits_.connections) {
    ++stats_.rejected_connections;
    return {};
  }
  ++stats_.admitted;
  account->admitted_ = true;
  return account;
}

TransportStats TransportBudget::Stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

ConnectionAccount::~ConnectionAccount() {
  if (!admitted_) return;
  std::lock_guard<std::mutex> lock(budget_->mutex_);
  --budget_->stats_.admitted;
}

bool ConnectionAccount::TryReserve(size_t bytes) {
  TransportStats stats;
  size_t retained;
  const char* reason;
  {
    std::lock_guard<std::mutex> lock(budget_->mutex_);
    if (failed_) return false;
    const auto& limits = budget_->limits_;
    reason = bytes > limits.connection_bytes - retained_ ? "connection" :
        bytes > limits.listener_bytes - budget_->stats_.retained ? "listener" : nullptr;
    if (!reason) {
      retained_ += bytes;
      auto& s = budget_->stats_;
      s.retained += bytes;
      s.peak = (std::max)(s.peak, s.retained);
      s.connection_peak = (std::max)(s.connection_peak, retained_);
      return true;
    }
    failed_ = true;
    ++budget_->stats_.overflows;
    stats = budget_->stats_;
    retained = retained_;
  }
  TRACEV8INSPECTOR_ERROR("Inspector transport overflow",
      TraceLoggingUInt64(bytes, "requested"), TraceLoggingUInt64(retained, "connectionRetained"),
      TraceLoggingUInt64(stats.retained, "listenerRetained"),
      TraceLoggingUInt64(budget_->limits_.connection_bytes, "connectionLimit"),
      TraceLoggingUInt64(budget_->limits_.listener_bytes, "listenerLimit"), TraceLoggingString(reason, "reason"));
  return false;
}

void ConnectionAccount::Release(size_t bytes) {
  std::lock_guard<std::mutex> lock(budget_->mutex_);
  retained_ -= bytes;
  budget_->stats_.retained -= bytes;
}

bool ConnectionAccount::Failed() {
  std::lock_guard<std::mutex> lock(budget_->mutex_);
  return failed_;
}

std::shared_ptr<OutboundBuffer> OutboundBuffer::Create(std::shared_ptr<ConnectionAccount> account, size_t bytes) {
  auto buffer = std::make_shared<OutboundBuffer>(ConstructionKey{}, std::move(account));
  // Saturation rejects unrepresentable payload + overhead against either budget.
  size_t charge = bytes > SIZE_MAX - kOverhead ? SIZE_MAX : bytes + kOverhead;
  if (!buffer->account_->TryReserve(charge)) return {};
  buffer->charge_ = charge; // Armed before storage allocation, including allocation failure.
  buffer->data.resize(bytes);
  return buffer;
}

OutboundBuffer::~OutboundBuffer() {
  std::vector<char>().swap(data);
  account_->Release(charge_);
}

#ifdef JSI_TESTING_ONLY
namespace {
std::mutex stats_mutex;
std::map<int, std::weak_ptr<TransportBudget>> port_budgets;
}
bool tcp_server::StatsForPort(int port, TransportStats& stats) {
  std::shared_ptr<TransportBudget> budget;
  {
    std::lock_guard<std::mutex> lock(stats_mutex);
    auto it = port_budgets.find(port);
    if (it != port_budgets.end()) budget = it->second.lock();
  }
  if (!budget) return false;
  stats = budget->Stats();
  return true;
}
#endif

tcp_server::tcp_server(int port, ConnectionCallback callback, void* data, TransportLimits limits)
  : budget_(std::make_shared<TransportBudget>(limits)),
    io_context_(), acceptor_(io_context_), socket_(io_context_), callbackData_(data), connectioncallback_(callback)
{
  asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), port);
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
#ifdef JSI_TESTING_ONLY
  {
    std::lock_guard<std::mutex> lock(stats_mutex);
    port_budgets[port] = budget_;
  }
#endif
  do_accept();
}

void tcp_server::run() {
  unsigned consecutive_failures = 0;
  while (!io_context_.stopped()) {
    auto completed_posts = completed_posts_;
    try {
      if (!io_context_.run_one()) break;
      consecutive_failures = 0;
      continue;
    } catch (std::exception const& e) {
      TRACEV8INSPECTOR_ERROR("io_context_.run", TraceLoggingString(e.what(), "message"));
    } catch (...) {
      TRACEV8INSPECTOR_ERROR("io_context_.run", TraceLoggingString("Unknown handler exception", "message"));
    }
    if (completed_posts_ != completed_posts) consecutive_failures = 0;
    if (++consecutive_failures == 8) {
      TRACEV8INSPECTOR_CRITICAL("io_context_.run",
          TraceLoggingString("Repeated failures without a completed handler", "message"));
      io_context_.stop();
    }
  }
  closeOnIoThread();
}

void tcp_server::post(std::function<void()> callback) {
#ifdef JSI_TESTING_ONLY
  ++posted_for_testing_;
#endif
  asio::post(strand_, [this, callback = std::move(callback)] {
    callback();
    ++completed_posts_;
  });
}

void tcp_server::closeOnIoThread() {
  asio::error_code ec;
  acceptor_.close(ec);
  socket_.close(ec);
  // EOF callbacks release the socket server's sessions on this same thread.
  auto connections = std::move(connections_);
  for (auto &weak : connections) {
    try {
      if (auto connection = weak.lock()) connection->eof();
    } catch (std::exception const& e) {
      TRACEV8INSPECTOR_ERROR("tcp_server.close", TraceLoggingString(e.what(), "message"));
    } catch (...) {
      TRACEV8INSPECTOR_ERROR("tcp_server.close", TraceLoggingString("Unknown EOF handler exception", "message"));
    }
  }
  io_context_.stop();
  {
    std::lock_guard<std::mutex> lock(stopped_mutex_);
    stopped_ = true;
  }
  stopped_cv_.notify_all();
}

void tcp_server::stop() {
  if (stopped_) return;
  if (io_context_.get_executor().running_in_this_thread()) {
    closeOnIoThread();
    return;
  }
  post([this] { closeOnIoThread(); });
  // Also signalled when run() itself reaches its failure cap.
  std::unique_lock<std::mutex> lock(stopped_mutex_);
  if (!stopped_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return stopped_.load(); })) {
    TRACEV8INSPECTOR_ERROR("tcp_server.stop", TraceLoggingString("Timed out waiting for io thread", "message"));
    // Socket operations remain confined to run() after the handler returns.
    io_context_.stop();
  }
}

void tcp_server::do_accept()
{
  std::shared_ptr<tcp_server> self;
  acceptor_.async_accept(socket_,
    [this, self](asio::error_code ec)
  {
    if (!ec)
    {
      auto account = budget_->TryAdmitConnection();
      if (!account) {
        TRACEV8INSPECTOR_WARNING("Inspector connection limit reached");
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        if (acceptor_.is_open()) do_accept();
        return;
      }
      auto connection = std::make_shared<tcp_connection>(std::move(socket_), std::move(account));
      connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
          [](const auto &weak) { return weak.expired(); }), connections_.end());
      connections_.push_back(connection);
      if (acceptor_.is_open()) do_accept();
      connectioncallback_(std::move(connection), callbackData_);
      return;
    }

    if (acceptor_.is_open()) do_accept();
  });
}

asio::ip::tcp::socket& tcp_connection::socket()
{
  return socket_;
}

tcp_connection::tcp_connection(asio::ip::tcp::socket socket, std::shared_ptr<ConnectionAccount> account)
    : socket_(std::move(socket)), account_(std::move(account)) {}

void tcp_connection::read_loop_async() {
  auto self(shared_from_this());
  socket_.async_read_some(asio::buffer(buffer_),
    [this, self](asio::error_code ec, std::size_t bytes_transferred)
  {
    if (!ec)
    {
      std::vector<char> vc;
      //const char* start = asio::buffer_cast<const char*>(input_buffer_.data());
      vc.reserve(bytes_transferred);
      for (size_t c = 0; c < bytes_transferred; c++) {
        vc.push_back(buffer_.data()[c]);
      }

      std::string s((buffer_.data()), bytes_transferred);

      if (readcallback_) readcallback_(vc, false, callbackData_);
      if (readcallback_ && !eof_) self->read_loop_async();
    }
    else if (ec != asio::error::operation_aborted) {
      eof();
    }
  });
}

void tcp_connection::write_raw(const char* data, size_t size) {
  auto buffer = OutboundBuffer::Create(account_, size);
  if (!buffer) { abort_async(); return; }
  if (size) std::copy_n(data, size, buffer->data.data());
  write_async(std::move(buffer));
}

void tcp_connection::abort_async() {
  if (aborting_ || eof_) return;
  aborting_ = true;
  outQueue = {};
  // Never re-enter an HTTP callback holding the socket server's session lock.
  asio::post(socket_.get_executor(), [self = shared_from_this()] { self->eof(); });
}

void tcp_connection::write_async(std::shared_ptr<OutboundBuffer> message_) {
  auto self = shared_from_this(); // eof() can release the connection's owner.
  if (eof_ || aborting_) return;
  if (account_->Failed()) { abort_async(); return; }
  if (!message_ || message_->data.empty()) {
    if (!writing_ && outQueue.empty()) {
      eof();
      return;
    }
    // Preserve queued messages for a reader, but bound a stalled disconnect.
    close_timer_.expires_after(std::chrono::seconds(2));
    close_timer_.async_wait([self](asio::error_code ec) { if (!ec) self->eof(); });
  }
  outQueue.push(std::move(message_));

  do_write(false);
}

void tcp_connection::do_write(bool cont) {

  // New message but the last write is going on.
  if (!cont && writing_) return;

  if (outQueue.empty()) {
    writing_ = false;
    return;
  }
  auto message = std::move(outQueue.front());
  outQueue.pop();
  writing_ = true;

  auto self(shared_from_this());

  if (!message || message->data.empty()) {
    writing_ = false;
    eof();
    return;
  }

  asio::async_write(socket_, asio::buffer(message->data),
    [self, message](asio::error_code ec, std::size_t)
  {
    if (self->eof_ || self->aborting_) return;
    if (self->account_->Failed()) { self->abort_async(); return; }
    if (!ec)
    {
      self->do_write(true);
    }

    if (ec == asio::error::operation_aborted) return;
    if (ec) {
      self->writing_ = false;
      self->eof();
    }
  });
}

void tcp_connection::eof() {
  if (eof_) return;
  eof_ = true;
  outQueue = {};
  close();
  std::vector<char> data;
  if (readcallback_) readcallback_(data, true, callbackData_);
}

void tcp_connection::close() {
  asio::error_code ec;
  close_timer_.cancel();
  // Send FIN before cancelling the outstanding read on Windows.
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
  socket_.close(ec);
  if (ec)
    std::abort();
}

}

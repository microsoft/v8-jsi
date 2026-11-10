// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#include "inspector_tcp.h"

#include <memory>
#include <algorithm>
#include "V8Windows.h"
#include <asio.hpp>

namespace inspector {

tcp_server::tcp_server(int port, ConnectionCallback callback, void* data)
  : io_context_(), acceptor_(io_context_), socket_(io_context_), callbackData_(data), connectioncallback_(callback)
{
  asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
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
      auto connection = std::make_shared<tcp_connection>(std::move(socket_));
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

void tcp_connection::write_async(std::vector<char>&& message_) {
  auto self = shared_from_this(); // eof() can release the connection's owner.
  if (eof_) return;
  if (message_.empty()) {
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
  messageToWrite_ = std::move(outQueue.front());
  outQueue.pop();
  writing_ = true;

  auto self(shared_from_this());

  if (messageToWrite_.empty()) {
    writing_ = false;
    eof();
    return;
  }

  std::string str;
  std::transform(messageToWrite_.begin(), messageToWrite_.end(), std::back_inserter(str), [](char c) { return c; });

  socket_.async_send(asio::buffer(messageToWrite_),
    [self](asio::error_code ec, std::size_t bytes_transferred)
  {
    if (!ec)
    {
      std::ostringstream stream;
      stream << "Writing completed .. " << bytes_transferred << " bytes.";

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

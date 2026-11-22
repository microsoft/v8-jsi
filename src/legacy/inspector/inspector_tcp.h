// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include <asio.hpp>

#include <queue>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace inspector {

struct TransportLimits {
  size_t connection_bytes{64 * 1024 * 1024};
  size_t listener_bytes{128 * 1024 * 1024};
  size_t connections{32};
};

struct TransportStats {
  size_t retained{0}, peak{0}, connection_peak{0};
  size_t admitted{0}, overflows{0}, rejected_connections{0};
};

// Observations from the test-only real-Send fixture; not a public ABI.
struct SendDispatchObservations {
  bool strand_blocked{false};
  TransportStats before_send, after_send, after_overflow, after_release;
  size_t posted_before{0}, posted_after_send{0}, posted_after_overflow{0}, posted_after_failure{0};
  size_t completed{0};
};

class ConnectionAccount;
class TransportBudget : public std::enable_shared_from_this<TransportBudget> {
 public:
  explicit TransportBudget(TransportLimits limits = {}) : limits_(limits) {}
  std::shared_ptr<ConnectionAccount> TryAdmitConnection();
  TransportStats Stats();
 private:
  friend class ConnectionAccount;
  const TransportLimits limits_;
  // Leaf lock: never invoke callbacks, logging, or socket operations under it.
  std::mutex mutex_;
  TransportStats stats_;
};

class ConnectionAccount {
 public:
  ~ConnectionAccount();
  ConnectionAccount(const ConnectionAccount&) = delete;
  ConnectionAccount& operator=(const ConnectionAccount&) = delete;
  bool TryReserve(size_t bytes);
  void Release(size_t bytes);
  bool Failed();
 private:
  friend class TransportBudget;
  explicit ConnectionAccount(std::shared_ptr<TransportBudget> budget) : budget_(std::move(budget)) {}
  const std::shared_ptr<TransportBudget> budget_;
  size_t retained_{0};
  bool admitted_{false};
  bool failed_{false};
};

// Single ownership of the charge, shared ownership of the buffer across dispatch
// and completion handlers. Payload storage is destroyed before refunding.
class OutboundBuffer {
 private:
  struct ConstructionKey { explicit ConstructionKey() = default; };
 public:
  // Covers the buffer/control block, queue slot, posted std::function and
  // allocator bookkeeping, conservatively rounded up for x86/x64/ARM64.
  static constexpr size_t kOverhead = 256;
  static std::shared_ptr<OutboundBuffer> Create(std::shared_ptr<ConnectionAccount> account, size_t bytes);
  explicit OutboundBuffer(ConstructionKey, std::shared_ptr<ConnectionAccount> account) : account_(std::move(account)) {}
  ~OutboundBuffer();
  OutboundBuffer(const OutboundBuffer&) = delete;
  OutboundBuffer& operator=(const OutboundBuffer&) = delete;
  std::vector<char> data;
 private:
  std::shared_ptr<ConnectionAccount> account_;
  size_t charge_{0};
};

class tcp_connection : public std::enable_shared_from_this<tcp_connection>
{
public:
  asio::ip::tcp::socket& socket();

  typedef void(*ReadCallback)(std::vector<char>&, bool iseof, void*data);
  inline void registerReadCallback(ReadCallback callback, void*data) { readcallback_ = callback; callbackData_ = data; }

  void read_loop_async();
  void write_async(std::shared_ptr<OutboundBuffer> message);
  void write_raw(const char* data, size_t size);
  void abort_async();
  std::shared_ptr<ConnectionAccount> account() const { return account_; }
  void close();

  explicit tcp_connection(asio::ip::tcp::socket socket, std::shared_ptr<ConnectionAccount> account);
private:
  friend class tcp_server;
  void do_write(bool cont);
  void eof();

private:
  asio::ip::tcp::socket socket_;
  const std::shared_ptr<ConnectionAccount> account_;
  asio::steady_timer close_timer_{socket_.get_executor()};
  std::string message_;

  /// Buffer for incoming data.
  std::array<char, 8192> buffer_;

  void* callbackData_{nullptr};
  ReadCallback readcallback_{nullptr};
  bool eof_{false};

  // Queue and write state are confined to the sole IO thread, including close.
  std::queue<std::shared_ptr<OutboundBuffer>> outQueue;

  bool aborting_{false};
  bool writing_{ false };
};

class tcp_server : public std::enable_shared_from_this<tcp_server> {
public:
  typedef void(*ConnectionCallback)(std::shared_ptr<tcp_connection> connection, void* callbackData_);

  void run();
  void stop();
  void post(std::function<void()> callback);

  tcp_server(int port, ConnectionCallback callback, void* data, TransportLimits limits = {});
  std::shared_ptr<TransportBudget> budget() const { return budget_; }
#ifdef JSI_TESTING_ONLY
  static bool StatsForPort(int port, TransportStats& stats);
  size_t PostedForTesting() const { return posted_for_testing_.load(); }
  size_t CompletedForTesting() const { return completed_posts_; } // Read after run() returns.
#endif

private:
  void do_accept();
  void closeOnIoThread();

private:
  std::atomic<bool> stopped_{false};
  std::mutex stopped_mutex_;
  std::condition_variable stopped_cv_;
  std::vector<std::weak_ptr<tcp_connection>> connections_;
  size_t completed_posts_{0}; // IO-thread-only progress inside batched strand dispatch.
#ifdef JSI_TESTING_ONLY
  std::atomic<size_t> posted_for_testing_{0};
#endif
  const std::shared_ptr<TransportBudget> budget_;
  asio::io_context io_context_;
  asio::strand<asio::io_context::executor_type> strand_{asio::make_strand(io_context_)};
  asio::ip::tcp::acceptor acceptor_;
  asio::ip::tcp::socket socket_;

  void* callbackData_;
  ConnectionCallback connectioncallback_;
};

}

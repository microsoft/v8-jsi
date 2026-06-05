// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include <stddef.h>
#include <memory>
#include <mutex>
#include <set>

#include <public/V8JsiRuntime.h>

namespace inspector {

class AgentImpl;

class Agent : public std::enable_shared_from_this<Agent> {
 public:
  explicit Agent(
      v8::Isolate *isolate,
      int port);
  ~Agent();

  void waitForDebugger();

  void addContext(v8::Local<v8::Context> context, const char* context_name);
  void removeContext(v8::Local<v8::Context> context);

  void start();
  void stop();

  bool IsStarted();
  bool IsConnected();
  void WaitForDisconnect();
  void FatalException(
      v8::Local<v8::Value> error,
      v8::Local<v8::Message> message);

  void notifyLoadedUrl(const std::string& url);
  std::shared_ptr<Agent> getShared();

  static void startAll();

 private:
  std::shared_ptr<AgentImpl> impl;

  // Tracks every AgentImpl that currently has at least one inspected context,
  // so the debugger-invoked startAll() can iterate live agents process-wide.
  // Held as weak_ptr so the set never extends AgentImpl lifetime past natural
  // V8Runtime teardown. Mutated/read concurrently by V8Runtime ctor/dtor on
  // arbitrary threads, so all access is serialized by agents_s_mutex_.
  static std::mutex agents_s_mutex_;
  static std::set<
      std::weak_ptr<inspector::AgentImpl>,
      std::owner_less<std::weak_ptr<inspector::AgentImpl>>>
      agents_s_;
};

} // namespace inspector

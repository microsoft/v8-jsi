// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include <stddef.h>
#include <unordered_set>

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
  static std::unordered_set<std::shared_ptr<inspector::AgentImpl>> agents_s_;
};

} // namespace inspector

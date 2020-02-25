// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#include "inspector_agent.h"
#include "inspector_socket_server.h"
#include "inspector_utils.h"

#include "v8-inspector.h"
#include "v8-platform.h"

#include <string.h>
#include <chrono>
#include <map>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <condition_variable>
#include <array>
#include <random>
#include <algorithm>

namespace inspector {

const char TAG_CONNECT[] = "#connect";
const char TAG_DISCONNECT[] = "#disconnect";

inline v8::Local<v8::String>
OneByteString(v8::Isolate *isolate, const char *data, int length = -1) {
  return v8::String::NewFromOneByte(
             isolate,
             reinterpret_cast<const uint8_t *>(data),
             v8::NewStringType::kNormal,
             length)
      .ToLocalChecked();
}

std::string GetProcessTitle() {
  return "ReactNativeWindows";
}

std::string GenerateID() {
  static std::random_device rd;
  static std::mt19937 mte(rd());

  std::uniform_int_distribution<uint16_t> dist;

  std::array<uint16_t, 8> buffer;
  std::generate(buffer.begin(), buffer.end(), [&] () { return dist(mte); });

  char uuid[256];
  snprintf(uuid, sizeof(uuid), "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
          buffer[0], buffer[1], buffer[2],
          (buffer[3] & 0x0fff) | 0x4000,
          (buffer[4] & 0x3fff) | 0x8000,
          buffer[5], buffer[6], buffer[7]);
  return uuid;
}

std::string StringViewToUtf8(const v8_inspector::StringView &view) {
  if (view.is8Bit()) {
    return std::string(
        reinterpret_cast<const char *>(view.characters8()), view.length());
  }

  return inspector::utils::utf16toUTF8(view.characters16(), view.length());
}

std::unique_ptr<v8_inspector::StringBuffer> Utf8ToStringView(
    const std::string &message) {
  std::wstring wstr =
      inspector::utils::Utf8ToUtf16(message.data(), message.length());
  v8_inspector::StringView view(
      reinterpret_cast<const uint16_t *>(wstr.c_str()), wstr.length());
  return v8_inspector::StringBuffer::create(view);
}

class V8NodeInspector;

class InspectorAgentDelegate : public inspector::SocketServerDelegate {
 public:
  InspectorAgentDelegate(
      AgentImpl *agent,
      const std::string &script_path,
      const std::string &script_name,
      bool wait);
  void AssignServer(InspectorSocketServer *server) override{};
  void StartSession(int session_id, const std::string &target_id) override;
  void MessageReceived(int session_id, const std::string &message) override;
  void EndSession(int session_id) override;
  std::vector<std::string> GetTargetIds() override;
  std::string GetTargetTitle(const std::string &id) override;
  std::string GetTargetUrl(const std::string &id) override;
  bool IsConnected() {
    return connected_;
  }

 private:
  AgentImpl *agent_;
  bool connected_;
  int session_id_;
  const std::string script_name_;
  const std::string script_path_;
  const std::string target_id_;
  bool waiting_;
};

class AgentImpl {
 public:
  explicit AgentImpl(
      v8::Platform &platform,
      v8::Isolate *isolate,
      v8::Local<v8::Context> context,
      const char *context_name,
      int port);
  ~AgentImpl();

  void Start();
  void Stop();

  void waitForDebugger();

  bool IsStarted();
  bool IsConnected();
  void WaitForDisconnect();

  void FatalException(
      v8::Local<v8::Value> error,
      v8::Local<v8::Message> message);

  void PostIncomingMessage(int session_id, const std::string &message);
  void ResumeStartup() {}

 private:
  using MessageQueue =
      std::vector<std::pair<int, std::unique_ptr<v8_inspector::StringBuffer>>>;
  enum class State { kNew, kAccepting, kConnected, kDone, kError };

  static void ThreadCbIO(void *agent);
  static void WriteCbIO(/*uv_async_t* async*/);

  void InstallInspectorOnProcess();

  void SetConnected(bool connected);
  void DispatchMessages();
  void Write(
      int session_id,
      std::unique_ptr<v8_inspector::StringBuffer> message);
  bool AppendMessage(
      MessageQueue *vector,
      int session_id,
      std::unique_ptr<v8_inspector::StringBuffer> buffer);
  void SwapBehindLock(MessageQueue *vector1, MessageQueue *vector2);
  void WaitForFrontendMessage();
  void NotifyMessageReceived();
  State ToState(State state);

  std::mutex incoming_message_cond_m_;
  std::condition_variable incoming_message_cond_;

  std::mutex state_m;

  InspectorAgentDelegate *delegate_;

  int port_;
  bool wait_;
  bool shutting_down_;
  State state_;

  bool waiting_for_frontend_ = true;

  V8NodeInspector *inspector_;
  v8::Isolate *isolate_;
  MessageQueue incoming_message_queue_;
  MessageQueue outgoing_message_queue_;
  bool dispatching_messages_;
  int session_id_;
  inspector::InspectorSocketServer *server_;

  std::string script_name_;

  v8::Platform &platform_;

  friend class ChannelImpl;
  friend class DispatchOnInspectorBackendTask;
  friend class SetConnectedTask;
  friend class V8NodeInspector;
  friend void InterruptCallback(v8::Isolate *, void *agent);

 public:
};

void InterruptCallback(v8::Isolate *, void *agent) {
  static_cast<AgentImpl *>(agent)->DispatchMessages();
}

class DispatchOnInspectorBackendTask : public v8::Task {
 public:
  explicit DispatchOnInspectorBackendTask(AgentImpl *agent) : agent_(agent) {}

  void Run() override {
    agent_->DispatchMessages();
  }

 private:
  AgentImpl *agent_;
};

class ChannelImpl final : public v8_inspector::V8Inspector::Channel {
 public:
  explicit ChannelImpl(AgentImpl *agent) : agent_(agent) {}
  virtual ~ChannelImpl() {}

 private:
  void sendResponse(
      int callId,
      std::unique_ptr<v8_inspector::StringBuffer> message) override {
    sendMessageToFrontend(std::move(message));
  }

  void sendNotification(
      std::unique_ptr<v8_inspector::StringBuffer> message) override {
    sendMessageToFrontend(std::move(message));
  }

  void flushProtocolNotifications() override {}

  void sendMessageToFrontend(
      std::unique_ptr<v8_inspector::StringBuffer> message) {
    agent_->Write(agent_->session_id_, std::move(message));
  }

  AgentImpl *const agent_;
};

using V8Inspector = v8_inspector::V8Inspector;

class V8NodeInspector : public v8_inspector::V8InspectorClient {
 public:
  V8NodeInspector(AgentImpl *agent)
      : agent_(agent),
        waiting_for_resume_(false),
        running_nested_loop_(false),
        inspector_(V8Inspector::create(agent->isolate_, this)) {}

  void setupContext(
      v8::Local<v8::Context> context,
      const char *context_name /*must be null terminated*/) {
    v8_inspector::V8ContextInfo info(
        context, 1, Utf8ToStringView(context_name)->string());

    std::unique_ptr<v8_inspector::StringBuffer> aux_data_buffer;
    aux_data_buffer = Utf8ToStringView("{\"isDefault\":true}");
    info.auxData = aux_data_buffer->string();

    inspector_->contextCreated(info);
  }

  void runMessageLoopOnPause(int context_group_id) override {
    waiting_for_resume_ = true;
    if (running_nested_loop_)
      return;
    running_nested_loop_ = true;
    while (waiting_for_resume_) {
      agent_->WaitForFrontendMessage();
      agent_->DispatchMessages();
    }
    waiting_for_resume_ = false;
    running_nested_loop_ = false;
  }

  double currentTimeMS() override {
    auto duration = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
        .count();
  }

  void quitMessageLoopOnPause() override {
    waiting_for_resume_ = false;
  }

  void connectFrontend() {
    session_ = inspector_->connect(
        1, new ChannelImpl(agent_), v8_inspector::StringView());
  }

  void disconnectFrontend() {
    session_.reset();
  }

  void dispatchMessageFromFrontend(const v8_inspector::StringView &message) {
    std::string messagestr = StringViewToUtf8(message);

    if (agent_->waiting_for_frontend_)
      agent_->waiting_for_frontend_ =
          messagestr.find("Runtime.runIfWaitingForDebugger") !=
          std::string::npos;

    session_->dispatchProtocolMessage(message);
  }

  v8::Local<v8::Context> ensureDefaultContextInGroup(
      int contextGroupId) override {
    return v8::Isolate::GetCurrent()->GetCurrentContext();
  }

  V8Inspector *inspector() {
    return inspector_.get();
  }

  bool isWaitingForResume() {
    return waiting_for_resume_;
  }

  std::unique_ptr<v8_inspector::V8InspectorSession> session_;

 private:
  AgentImpl *agent_;
  v8::Platform *platform_;
  std::atomic<bool> waiting_for_resume_ {false};
  bool running_nested_loop_;
  std::unique_ptr<V8Inspector> inspector_;
};

AgentImpl::AgentImpl(
    v8::Platform &platform,
    v8::Isolate *isolate,
    v8::Local<v8::Context> context,
    const char *context_name,
    int port)
    : platform_(platform),
      isolate_(isolate),
      port_(port),
      wait_(false),
      shutting_down_(false),
      state_(State::kNew),
      inspector_(nullptr),
      dispatching_messages_(false),
      session_id_(0),
      server_(nullptr) {
  inspector_ = new V8NodeInspector(this);
  inspector_->setupContext(context, context_name);
}

AgentImpl::~AgentImpl() {}

void InspectorConsoleCall(const v8::FunctionCallbackInfo<v8::Value> &info) {
  v8::Isolate *isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();

  v8::Local<v8::Array> args = info.Data().As<v8::Array>();
  CHECK_EQ(args->Length(), 3);

  v8::Local<v8::Value> inspector_method =
      args->Get(context, 0).ToLocalChecked();
  CHECK(inspector_method->IsFunction());
  v8::Local<v8::Value> node_method = args->Get(context, 1).ToLocalChecked();
  CHECK(node_method->IsFunction());
  v8::Local<v8::Value> config_value = args->Get(context, 2).ToLocalChecked();
  CHECK(config_value->IsObject());
  v8::Local<v8::Object> config_object = config_value.As<v8::Object>();

  std::vector<v8::Local<v8::Value>> call_args(info.Length());
  for (int i = 0; i < info.Length(); ++i) {
    call_args[i] = info[i];
  }

  v8::Local<v8::String> in_call_key = OneByteString(isolate, "in_call");
  bool in_call = config_object->Has(context, in_call_key).FromMaybe(false);
  if (!in_call) {
    CHECK(
        config_object->Set(context, in_call_key, v8::True(isolate)).FromJust());
    CHECK(
        !inspector_method.As<v8::Function>()
             ->Call(context, info.Holder(), call_args.size(), call_args.data())
             .IsEmpty());
  }

  v8::TryCatch try_catch(info.GetIsolate());
  static_cast<void>(node_method.As<v8::Function>()->Call(
      context, info.Holder(), call_args.size(), call_args.data()));
  CHECK(config_object->Delete(context, in_call_key).FromJust());
  if (try_catch.HasCaught())
    try_catch.ReThrow();
}

void InspectorWrapConsoleCall(const v8::FunctionCallbackInfo<v8::Value> &args) {


  v8::Local<v8::Array> array =
      v8::Array::New(v8::Isolate::GetCurrent(), args.Length());
  CHECK(array->Set(v8::Isolate::GetCurrent()->GetCurrentContext(), 0, args[0])
            .FromJust());
  CHECK(array->Set(v8::Isolate::GetCurrent()->GetCurrentContext(), 1, args[1])
            .FromJust());
  CHECK(array->Set(v8::Isolate::GetCurrent()->GetCurrentContext(), 2, args[2])
            .FromJust());
  args.GetReturnValue().Set(v8::Function::New(
                                v8::Isolate::GetCurrent()->GetCurrentContext(),
                                InspectorConsoleCall,
                                array)
                                .ToLocalChecked());
}

void AgentImpl::Start() {
  std::thread([this]() {
    InspectorAgentDelegate delegate(this, "", script_name_, wait_);
    delegate_ = &delegate;

    InspectorSocketServer server(
        std::unique_ptr<InspectorAgentDelegate>(&delegate), port_);
    server_ = &server;

    // This loops
    if (!server.Start()) {
      std::abort();
    }
  })
      .detach();
}

void AgentImpl::waitForDebugger() {
  WaitForFrontendMessage();

  if (state_ == State::kError) {
    Stop();
  }
  state_ = State::kAccepting;

  while (waiting_for_frontend_)
    DispatchMessages();

  std::string reasonstr("Break on start");
  v8_inspector::StringView reason(
      reinterpret_cast<const uint8_t *>(reasonstr.c_str()), reasonstr.size()),
      details(
          reinterpret_cast<const uint8_t *>(reasonstr.c_str()),
          reasonstr.size());
  inspector_->session_->schedulePauseOnNextStatement(reason, details);
}

void AgentImpl::Stop() {
  delete inspector_;
}

bool AgentImpl::IsConnected() {
  return delegate_ != nullptr && delegate_->IsConnected();
}

bool AgentImpl::IsStarted() {
  return true;
}

void AgentImpl::WaitForDisconnect() {
  if (state_ == State::kConnected) {
    shutting_down_ = true;
    // Gives a signal to stop accepting new connections
    // TODO(eugeneo): Introduce an API with explicit request names.
    Write(0, v8_inspector::StringBuffer::create((v8_inspector::StringView())));
    fprintf(stderr, "Waiting for the debugger to disconnect...\n");
    fflush(stderr);
    inspector_->runMessageLoopOnPause(0);
  }
}

std::unique_ptr<v8_inspector::StringBuffer> ToProtocolString(
    v8::Local<v8::Value> value) {
  if (value.IsEmpty() || value->IsNull() || value->IsUndefined() ||
      !value->IsString()) {
    return v8_inspector::StringBuffer::create(v8_inspector::StringView());
  }
  v8::Local<v8::String> string_value = v8::Local<v8::String>::Cast(value);
  size_t len = string_value->Length();
  std::basic_string<uint16_t> buffer(len, '\0');
  string_value->Write(v8::Isolate::GetCurrent(), &buffer[0], 0, len);
  return v8_inspector::StringBuffer::create(
      v8_inspector::StringView(buffer.data(), len));
}

void AgentImpl::FatalException(
    v8::Local<v8::Value> error,
    v8::Local<v8::Message> message) {
  if (!IsStarted())
    return;
  v8::Local<v8::Context> context =
      v8::Isolate::GetCurrent()->GetCurrentContext();

  int script_id = message->GetScriptOrigin().ScriptID()->Value();

  v8::Local<v8::StackTrace> stack_trace = message->GetStackTrace();

  if (!stack_trace.IsEmpty() && stack_trace->GetFrameCount() > 0 &&
      script_id ==
          stack_trace->GetFrame(v8::Isolate::GetCurrent(), 0)->GetScriptId()) {
    script_id = 0;
  }

  const uint8_t DETAILS[] = "Uncaught";

  inspector_->inspector()->exceptionThrown(
      context,
      v8_inspector::StringView(DETAILS, sizeof(DETAILS) - 1),
      error,
      ToProtocolString(message->Get())->string(),
      ToProtocolString(message->GetScriptResourceName())->string(),
      message->GetLineNumber(context).FromMaybe(0),
      message->GetStartColumn(context).FromMaybe(0),
      inspector_->inspector()->createStackTrace(stack_trace),
      script_id);
  WaitForDisconnect();
}

bool AgentImpl::AppendMessage(
    MessageQueue *queue,
    int session_id,
    std::unique_ptr<v8_inspector::StringBuffer> buffer) {
  std::unique_lock<std::mutex> lock(state_m);
  bool trigger_pumping = queue->empty();
  queue->push_back(std::make_pair(session_id, std::move(buffer)));
  return trigger_pumping;
}

void AgentImpl::SwapBehindLock(MessageQueue *vector1, MessageQueue *vector2) {
  std::unique_lock<std::mutex> lock(state_m);
  vector1->swap(*vector2);
}

void AgentImpl::PostIncomingMessage(
    int session_id,
    const std::string &message) {
  if (AppendMessage(
          &incoming_message_queue_, session_id, Utf8ToStringView(message))) {
    platform_.CallOnForegroundThread(
        isolate_, new DispatchOnInspectorBackendTask(this));
    isolate_->RequestInterrupt(InterruptCallback, this);
  }
  NotifyMessageReceived();
}

void AgentImpl::WaitForFrontendMessage() {
  std::unique_lock<std::mutex> lock(incoming_message_cond_m_);
  if (incoming_message_queue_.empty())
    incoming_message_cond_.wait(
        lock, [this] { return !incoming_message_queue_.empty(); });
}

void AgentImpl::NotifyMessageReceived() {
  incoming_message_cond_.notify_all();
}

void AgentImpl::DispatchMessages() {
  // This function can be reentered if there was an incoming message while
  // V8 was processing another inspector request (e.g. if the user is
  // evaluating a long-running JS code snippet). This can happen only at
  // specific points (e.g. the lines that call inspector_ methods)
  if (dispatching_messages_)
    return;
  dispatching_messages_ = true;
  MessageQueue tasks;
  do {
    tasks.clear();
    SwapBehindLock(&incoming_message_queue_, &tasks);
    for (const MessageQueue::value_type &pair : tasks) {
      v8_inspector::StringView message = pair.second->string();
      std::string tag;
      if (message.length() == sizeof(TAG_CONNECT) - 1 ||
          message.length() == sizeof(TAG_DISCONNECT) - 1) {
        tag = StringViewToUtf8(message);
      }

      if (tag == TAG_CONNECT) {
        CHECK_EQ(State::kAccepting, state_);
        session_id_ = pair.first;
        state_ = State::kConnected;
        inspector_->connectFrontend();
      } else if (tag == TAG_DISCONNECT) {
        CHECK_EQ(State::kConnected, state_);
        if (shutting_down_) {
          state_ = State::kDone;
        } else {
          state_ = State::kAccepting;
        }

        inspector_->quitMessageLoopOnPause();
        inspector_->disconnectFrontend();
      } else {
        inspector_->dispatchMessageFromFrontend(message);
      }
    }
  } while (!tasks.empty());
  dispatching_messages_ = false;
}

void AgentImpl::Write(
    int session_id,
    std::unique_ptr<v8_inspector::StringBuffer> inspector_message) {
  AppendMessage(
      &outgoing_message_queue_, session_id, std::move(inspector_message));

  MessageQueue outgoing_messages;
  SwapBehindLock(&outgoing_message_queue_, &outgoing_messages);
  for (const MessageQueue::value_type &outgoing : outgoing_messages) {
    v8_inspector::StringView view = outgoing.second->string();
    if (view.length() == 0) {
      server_->Stop();
    } else {
      server_->Send(
          outgoing.first, StringViewToUtf8(outgoing.second->string()));
    }
  }
}

// Exported class Agent
Agent::Agent(
    v8::Platform &platform,
    v8::Isolate *isolate,
    v8::Local<v8::Context> context,
    const char *context_name,
    int port)
    : impl(new AgentImpl(platform, isolate, context, context_name, port)) {}

Agent::~Agent() {
  delete impl;
}

void Agent::waitForDebugger() {
  impl->waitForDebugger();
}

void Agent::stop() {
  impl->Stop();
}

void Agent::start() {
  impl->Start();
}

bool Agent::IsStarted() {
  return impl->IsStarted();
}

bool Agent::IsConnected() {
  return impl->IsConnected();
}

void Agent::WaitForDisconnect() {
  impl->WaitForDisconnect();
}

void Agent::FatalException(
    v8::Local<v8::Value> error,
    v8::Local<v8::Message> message) {
  impl->FatalException(error, message);
}

InspectorAgentDelegate::InspectorAgentDelegate(
    AgentImpl *agent,
    const std::string &script_path,
    const std::string &script_name,
    bool wait)
    : agent_(agent),
      connected_(false),
      session_id_(0),
      script_name_(script_name),
      script_path_(script_path),
      target_id_(GenerateID()),
      waiting_(wait) {}

void InspectorAgentDelegate::StartSession(
    int session_id,
    const std::string & /*target_id*/) {
  connected_ = true;
  agent_->PostIncomingMessage(session_id, TAG_CONNECT);
}

void InspectorAgentDelegate::MessageReceived(
    int session_id,
    const std::string &message) {
  // TODO(pfeldman): Instead of blocking execution while debugger
  // engages, node should wait for the run callback from the remote client
  // and initiate its startup. This is a change to node.cc that should be
  // upstreamed separately.
  if (waiting_) {
    if (message.find("\"Runtime.runIfWaitingForDebugger\"") !=
        std::string::npos) {
      waiting_ = false;
      agent_->ResumeStartup();
    }
  }
  agent_->PostIncomingMessage(session_id, message);
}

void InspectorAgentDelegate::EndSession(int session_id) {
  connected_ = false;
  agent_->PostIncomingMessage(session_id, TAG_DISCONNECT);
}

std::vector<std::string> InspectorAgentDelegate::GetTargetIds() {
  return {target_id_};
}

std::string InspectorAgentDelegate::GetTargetTitle(const std::string &id) {
  return script_name_.empty() ? GetProcessTitle() : script_name_;
}

std::string InspectorAgentDelegate::GetTargetUrl(const std::string &id) {
  return "file://" + script_path_;
}

} // namespace inspector
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

#include "V8Windows.h"

#include "../V8Platform.h"

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
  return "V8JsiHost";
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

class AgentImpl : public std::enable_shared_from_this<AgentImpl> {
 public:
  explicit AgentImpl(
      v8::Platform &platform,
      v8::Isolate *isolate,
      v8::Local<v8::Context> context,
      const char *context_name,
      int port);
  ~AgentImpl();

  InspectorSocketServer& ensureServer();

  void Start();
  void Stop();

  void waitForDebugger();

  bool IsStarted();
  // bool IsConnected();
  void WaitForDisconnect();

  void notifyLoadedUrl(const std::string& url);

  void FatalException(
      v8::Local<v8::Value> error,
      v8::Local<v8::Message> message);

  void PostIncomingMessage(int session_id, const std::string &message);
  void ResumeStartup() {}

  std::string getTitle();

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

  int port_;
  bool wait_;
  bool shutting_down_;
  State state_;

  bool waiting_for_frontend_ = true;

  std::unique_ptr<V8NodeInspector> inspector_;
  v8::Isolate *isolate_;
  MessageQueue incoming_message_queue_;
  MessageQueue outgoing_message_queue_;
  bool dispatching_messages_;
  int session_id_;

  static std::mutex g_mutex_server_init_;
  static std::unordered_map<int, std::unique_ptr<inspector::InspectorSocketServer>> g_servers_;

  std::string title_;
  std::string loaded_urls_;

  v8::Platform &platform_;

  friend class ChannelImpl;
  friend class DispatchOnInspectorBackendTask;
  friend class SetConnectedTask;
  friend class V8NodeInspector;
  friend void InterruptCallback(v8::Isolate *, void *agent);

 public:
};

/*static*/ std::mutex AgentImpl::g_mutex_server_init_;
/*static*/ std::unordered_map < int,
    std::unique_ptr<inspector::InspectorSocketServer>> AgentImpl::g_servers_;

void InterruptCallback(v8::Isolate *, void *agent) {
  static_cast<AgentImpl *>(agent)->DispatchMessages();
}

class DispatchOnInspectorBackendTask : public v8::Task {
 public:
  explicit DispatchOnInspectorBackendTask(AgentImpl& agent) : agent_(agent) {}

  void Run() override {
    agent_.DispatchMessages();
  }

 private:
  AgentImpl& agent_;
};

class ChannelImpl final : public v8_inspector::V8Inspector::Channel {
 public:
  explicit ChannelImpl(AgentImpl& agent) : agent_(agent) {}
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
    agent_.Write(agent_.session_id_, std::move(message));
  }

  AgentImpl& agent_;
};

using V8Inspector = v8_inspector::V8Inspector;

class V8NodeInspector : public v8_inspector::V8InspectorClient {
 public:
  V8NodeInspector(AgentImpl& agent)
      : agent_(agent),
        waiting_for_resume_(false),
        running_nested_loop_(false),
        inspector_(V8Inspector::create(agent.isolate_, this)) {}

  void setupContext(
      v8::Local<v8::Context> context,
      const char *context_name /*must be null terminated*/) {
    std::unique_ptr<v8_inspector::StringBuffer> name_buffer = Utf8ToStringView(context_name);
    v8_inspector::V8ContextInfo info(context, 1, name_buffer->string());

    std::unique_ptr<v8_inspector::StringBuffer> aux_data_buffer = Utf8ToStringView("{\"isDefault\":true}");
    info.auxData = aux_data_buffer->string();

    inspector_->contextCreated(info);
  }

  void runMessageLoopOnPause(int context_group_id) override {
    waiting_for_resume_ = true;
    if (running_nested_loop_)
      return;
    running_nested_loop_ = true;
    while (waiting_for_resume_) {
      agent_.WaitForFrontendMessage();
      agent_.DispatchMessages();
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

    if (agent_.waiting_for_frontend_)
      agent_.waiting_for_frontend_ =
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
  AgentImpl& agent_;
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
      session_id_(0) {
  inspector_ = std::make_unique<V8NodeInspector>(*this);
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
             ->Call(context, info.Holder(), static_cast<int>(call_args.size()), call_args.data())
             .IsEmpty());
  }

  v8::TryCatch try_catch(info.GetIsolate());
  static_cast<void>(node_method.As<v8::Function>()->Call(
      context, info.Holder(), static_cast<int>(call_args.size()), call_args.data()));
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


InspectorSocketServer& AgentImpl::ensureServer() {
  const std::lock_guard<std::mutex> lock(g_mutex_server_init_);

  auto server = g_servers_.find(port_);
  if (server == g_servers_.end()) {
    auto delegate = std::make_unique<InspectorAgentDelegate>();
    auto newserver =
        std::make_unique<InspectorSocketServer>(std::move(delegate), port_);
    if (!newserver->Start()) {
      std::abort();
    }

    g_servers_[port_] = std::move(newserver);
  }

  return *g_servers_[port_];

}

void AgentImpl::Start() {
  auto& server = ensureServer();
  state_ = State::kAccepting;
  server.AddTarget(shared_from_this());
}

void AgentImpl::waitForDebugger() {
  // TODO:: We should create more discrete events, and add the text as argument.
  TRACEV8INSPECTOR_VERBOSE("Waiting for frontend message");
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
  TRACEV8INSPECTOR_VERBOSE("Resuming after frontend attached.");
}


void AgentImpl::Stop() {
  std::string msg("{\"method\":\"Runtime.executionContextsCleared\"}");
  Write(session_id_,
        v8_inspector::StringBuffer::create((v8_inspector::StringView(
            reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length()))));

  Write(session_id_,
        v8_inspector::StringBuffer::create((v8_inspector::StringView(
            reinterpret_cast<const uint8_t*>(""), 0))));

  WaitForDisconnect();
  inspector_.reset();
}

//bool AgentImpl::IsConnected() {
//  auto& server = ensureServer();
//  return server.IsConnected();
//}

bool AgentImpl::IsStarted() {
  return true;
}

void AgentImpl::WaitForDisconnect() {
  if (state_ == State::kConnected) {
    shutting_down_ = true;
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
  int len = string_value->Length();
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

  std::shared_ptr<v8::TaskRunner> foregroundTaskRunner;

#ifdef USE_DEFAULT_PLATFORM
    // Need to get the foreground runner from the isolate data slot
    v8runtime::IsolateData* isolate_data = reinterpret_cast<v8runtime::IsolateData*>(isolate_->GetData(v8runtime::ISOLATE_DATA_SLOT));
    foregroundTaskRunner = isolate_data->foreground_task_runner_;
#else
    foregroundTaskRunner = platform_.GetForegroundTaskRunner(isolate_);
#endif
    foregroundTaskRunner->PostTask(std::make_unique<DispatchOnInspectorBackendTask>(*this));
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

        TRACEV8INSPECTOR_VERBOSE(
            "InMessage",
            TraceLoggingCountedString16(
                reinterpret_cast<const char16_t*>(message.characters16()), message.length(), "message"));


        inspector_->dispatchMessageFromFrontend(message);
      }
    }
  } while (!tasks.empty());
  dispatching_messages_ = false;
}

void AgentImpl::Write(
    int session_id,
    std::unique_ptr<v8_inspector::StringBuffer> inspector_message) {
  AppendMessage(&outgoing_message_queue_, session_id,
                std::move(inspector_message));

  MessageQueue outgoing_messages;
  SwapBehindLock(&outgoing_message_queue_, &outgoing_messages);
  for (const MessageQueue::value_type& outgoing : outgoing_messages) {
    v8_inspector::StringView view = outgoing.second->string();
    std::string message = StringViewToUtf8(outgoing.second->string());
    TRACEV8INSPECTOR_VERBOSE("OutMessage",
                             TraceLoggingString(message.c_str(), "message"));

    ensureServer().Send(outgoing.first, std::move(message));
  }
}

void AgentImpl::notifyLoadedUrl(const std::string& url) {
  // We are trying to constuct a title shown on the inspector UI using the
  // loaded script urls. Find upto last three segments from the url ...
  std::string new_url_sufix;
  std::size_t found = url.find_last_of("/\\");
  if (found == std::string::npos) {
    new_url_sufix.append(url);
  } else {
    found = url.find_last_of("/\\", found - 1);
    if (found == std::string::npos) {
      new_url_sufix.append("...");
      new_url_sufix.append(url);
    } else {
      found = url.find_last_of("/\\", found - 1);
      if (found == std::string::npos) {
        new_url_sufix.append("...");
        new_url_sufix.append(url);
      } else {
        new_url_sufix.append("...");
        new_url_sufix.append(url.substr(found + 1));
      }
    }
  }

  if (loaded_urls_.size() > 0) loaded_urls_.append(", ");
  loaded_urls_.append(new_url_sufix);
  title_ = "V8JSI Host(" + loaded_urls_ + ")";
}

std::string AgentImpl::getTitle() { return title_; }

// Exported class Agent
Agent::Agent(
    v8::Platform &platform,
    v8::Isolate *isolate,
    v8::Local<v8::Context> context,
    const char *context_name,
    int port)
    : impl(std::make_shared<AgentImpl>(platform, isolate, context, context_name,
                                       port)) {}

Agent::~Agent() {
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

//bool Agent::IsConnected() {
//  return impl->IsConnected();
//}

void Agent::WaitForDisconnect() {
  impl->WaitForDisconnect();
}

void Agent::notifyLoadedUrl(const std::string& url) { impl->notifyLoadedUrl(url); }

void Agent::FatalException(
    v8::Local<v8::Value> error,
    v8::Local<v8::Message> message) {
  impl->FatalException(error, message);
}

InspectorAgentDelegate::InspectorAgentDelegate(){}

void InspectorAgentDelegate::StartSession(
    int session_id,
    const std::string & target_id) {
  auto agent = targets_map_[target_id];
  session_targets_map_.emplace(session_id, agent);
  agent->PostIncomingMessage(session_id, TAG_CONNECT);
}

namespace {
std::string GenerateID() {
  static std::random_device rd;
  static std::mt19937 mte(rd());

  std::uniform_int_distribution<uint16_t> dist;

  std::array<uint16_t, 8> buffer;
  std::generate(buffer.begin(), buffer.end(), [&]() { return dist(mte); });

  char uuid[256];
  snprintf(uuid, sizeof(uuid), "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
           buffer[0], buffer[1], buffer[2], (buffer[3] & 0x0fff) | 0x4000,
           (buffer[4] & 0x3fff) | 0x8000, buffer[5], buffer[6], buffer[7]);
  return uuid;
}
}  // namespace

void InspectorAgentDelegate::MessageReceived(
    int session_id,
    const std::string &message) {
  session_targets_map_[session_id]->PostIncomingMessage(session_id, message);
}

void InspectorAgentDelegate::EndSession(int session_id) {
  // connected_ = false;
  session_targets_map_[session_id]->PostIncomingMessage(session_id,
                                                       TAG_DISCONNECT);
}


void InspectorAgentDelegate::AddTarget(std::shared_ptr<AgentImpl> agent) {
  std::string targetId = GenerateID();
  targets_map_.emplace(targetId, agent);
}

std::vector<std::string> InspectorAgentDelegate::GetTargetIds() {
  std::vector<std::string> keys;
  keys.reserve(targets_map_.size());
  
  for (auto kv : targets_map_) {
    keys.push_back(kv.first);
  }

  return keys;
}

std::string InspectorAgentDelegate::GetTargetTitle(const std::string& id) {
  auto agent = targets_map_[id];
  return agent->getTitle();
}

std::string InspectorAgentDelegate::GetTargetUrl(const std::string &id) {
  return "file://" + id;
}

} // namespace inspector

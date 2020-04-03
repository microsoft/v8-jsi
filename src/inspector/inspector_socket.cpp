// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#include "inspector_tcp.h"
#include "inspector_socket.h"
#include "inspector_utils.h"

#include "llhttp.h"
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstring>

typedef llhttp_type_t parser_type_t;
typedef llhttp_errno_t parser_errno_t;
typedef llhttp_settings_t parser_settings_t;
typedef llhttp_t parser_t;

// The helper is for doing safe downcasts from base types to derived types.
template <typename Inner, typename Outer>
class ContainerOfHelper {
public:
  inline ContainerOfHelper(Inner Outer::*field, Inner* pointer);
  template <typename TypeName>
  inline operator TypeName*() const;
private:
  Outer* const pointer_;
};

template <typename Inner, typename Outer>
constexpr uintptr_t OffsetOf(Inner Outer::*field) {
  return reinterpret_cast<uintptr_t>(&(static_cast<Outer*>(0)->*field));
}

template <typename Inner, typename Outer>
ContainerOfHelper<Inner, Outer>::ContainerOfHelper(Inner Outer::*field,
  Inner* pointer)
  : pointer_(
    reinterpret_cast<Outer*>(
      reinterpret_cast<uintptr_t>(pointer) - OffsetOf(field))) {}

template <typename Inner, typename Outer>
template <typename TypeName>
ContainerOfHelper<Inner, Outer>::operator TypeName*() const {
  return static_cast<TypeName*>(pointer_);
}

template <typename Inner, typename Outer>
constexpr ContainerOfHelper<Inner, Outer> ContainerOf(Inner Outer::*field,
  Inner* pointer) {
  return ContainerOfHelper<Inner, Outer>(field, pointer);
}

#include <map>
#include <string.h>

#define ACCEPT_KEY_LENGTH inspector::utils::base64_encoded_size(20)
#define BUFFER_GROWTH_CHUNK_SIZE 1024

#define DUMP_READS 0
#define DUMP_WRITES 0

namespace inspector {

class TcpHolder {
 public:
  static void DisconnectAndDispose(TcpHolder* holder);
  using Pointer = DeleteFnPtr<TcpHolder, DisconnectAndDispose>;

  static Pointer Accept(tcp_connection::pointer socket, InspectorSocket::DelegatePointer delegate);
  void SetHandler(ProtocolHandler* handler);
  int WriteRaw(const std::vector<char>& buffer/*, uv_write_cb write_cb*/);

  void read_loop();

  std::shared_ptr<tcp_connection> connection() { return connection_; };
  InspectorSocket::Delegate* delegate();

  static void OnClosedCallback(void*data);

 private:
   static void OnDataReceivedCb(std::vector<char>&, bool iseof, void*data);

  
   tcp_connection::pointer connection_;

  explicit TcpHolder(std::shared_ptr<tcp_connection> connection, InspectorSocket::DelegatePointer delegate);
  ~TcpHolder() = default;
   
   const InspectorSocket::DelegatePointer delegate_;
  ProtocolHandler* handler_;
  std::vector<char> buffer_;
};


class ProtocolHandler {
 public:
  ProtocolHandler(InspectorSocket* inspector, TcpHolder::Pointer tcp);

  virtual void AcceptUpgrade(const std::string& accept_key) = 0;
  virtual void OnData(std::vector<char>* data) = 0;
  virtual void OnEof() = 0;
  virtual void Write(const std::vector<char> data) = 0;
  virtual void CancelHandshake() = 0;

  std::string GetHost() const;

  InspectorSocket* inspector() {
    return inspector_;
  }
  virtual void Shutdown() = 0;

 protected:
  virtual ~ProtocolHandler() = default;
  int WriteRaw(const std::vector<char>& buffer/*, uv_write_cb write_cb*/);
  InspectorSocket::Delegate* delegate();

  InspectorSocket* const inspector_;
  TcpHolder::Pointer tcp_;
};

namespace {

#if DUMP_READS || DUMP_WRITES
static void dump_hex(const char* buf, size_t len) {
  const char* ptr = buf;
  const char* end = ptr + len;
  const char* cptr;
  char c;
  int i;

  while (ptr < end) {
    cptr = ptr;
    for (i = 0; i < 16 && ptr < end; i++) {
      printf("%2.2X  ", static_cast<unsigned char>(*(ptr++)));
    }
    for (i = 72 - (i * 4); i > 0; i--) {
      printf(" ");
    }
    for (i = 0; i < 16 && cptr < end; i++) {
      c = *(cptr++);
      printf("%c", (c > 0x19) ? c : '.');
    }
    printf("\n");
  }
  printf("\n\n");
}
#endif

static void remove_from_beginning(std::vector<char>* buffer, size_t count) {
  buffer->erase(buffer->begin(), buffer->begin() + count);
}

static const char CLOSE_FRAME[] = {'\x88', '\x00'};

enum ws_decode_result {
  FRAME_OK, FRAME_INCOMPLETE, FRAME_CLOSE, FRAME_ERROR
};

static void generate_accept_string(const std::string& client_key,
                                   char (*buffer)[ACCEPT_KEY_LENGTH]) {
  // Magic string from websockets spec.
  static const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string input(client_key + ws_magic);

  inspector::utils::sha1_context ctx;
  init(ctx);
  update(ctx, input.data(), input.size());
  char digest[inspector::utils::sha1_context::digest_size];
  finish(ctx, &digest[0]);
  
  inspector::utils::base64_encode(digest, inspector::utils::sha1_context::digest_size, *buffer, sizeof(*buffer));
}

static std::string TrimPort(const std::string& host) {
  size_t last_colon_pos = host.rfind(":");
  if (last_colon_pos == std::string::npos)
    return host;
  size_t bracket = host.rfind("]");
  if (bracket == std::string::npos || last_colon_pos > bracket)
    return host.substr(0, last_colon_pos);
  return host;
}

static bool IsIPAddress(const std::string& host) {
  if (host.length() >= 4 && host.front() == '[' && host.back() == ']')
    return true;
  int quads = 0;
  for (char c : host) {
    if (c == '.')
      quads++;
    else if (!isdigit(c))
      return false;
  }
  return quads == 3;
}

// Constants for hybi-10 frame format.

typedef int OpCode;

const OpCode kOpCodeContinuation = 0x0;
const OpCode kOpCodeText = 0x1;
const OpCode kOpCodeBinary = 0x2;
const OpCode kOpCodeClose = 0x8;
const OpCode kOpCodePing = 0x9;
const OpCode kOpCodePong = 0xA;

const unsigned char kFinalBit = 0x80;
const unsigned char kReserved1Bit = 0x40;
const unsigned char kReserved2Bit = 0x20;
const unsigned char kReserved3Bit = 0x10;
const unsigned char kOpCodeMask = 0xF;
const unsigned char kMaskBit = 0x80;
const unsigned char kPayloadLengthMask = 0x7F;

const size_t kMaxSingleBytePayloadLength = 125;
const size_t kTwoBytePayloadLengthField = 126;
const size_t kEightBytePayloadLengthField = 127;
const size_t kMaskingKeyWidthInBytes = 4;

static std::vector<char> encode_frame_hybi17(const std::vector<char>& message) {
  std::vector<char> frame;
  OpCode op_code = kOpCodeText;
  frame.push_back(kFinalBit | op_code);
  const size_t data_length = message.size();
  if (data_length <= kMaxSingleBytePayloadLength) {
    frame.push_back(static_cast<char>(data_length));
  } else if (data_length <= 0xFFFF) {
    frame.push_back(kTwoBytePayloadLengthField);
    frame.push_back((data_length & 0xFF00) >> 8);
    frame.push_back(data_length & 0xFF);
  } else {
    frame.push_back(kEightBytePayloadLengthField);
    char extended_payload_length[8];
    size_t remaining = data_length;
    // Fill the length into extended_payload_length in the network byte order.
    for (int i = 0; i < 8; ++i) {
      extended_payload_length[7 - i] = remaining & 0xFF;
      remaining >>= 8;
    }
    frame.insert(frame.end(), extended_payload_length,
                 extended_payload_length + 8);
    CHECK_EQ(0, remaining);
  }
  frame.insert(frame.end(), message.begin(), message.end());
  return frame;
}

static ws_decode_result decode_frame_hybi17(const std::vector<char>& buffer,
                                            bool client_frame,
                                            size_t* bytes_consumed,
                                            std::vector<char>* output,
                                            bool* compressed) {
  *bytes_consumed = 0;
  if (buffer.size() < 2)
    return FRAME_INCOMPLETE;

  auto it = buffer.begin();

  unsigned char first_byte = *it++;
  unsigned char second_byte = *it++;

  bool final = (first_byte & kFinalBit) != 0;
  bool reserved1 = (first_byte & kReserved1Bit) != 0;
  bool reserved2 = (first_byte & kReserved2Bit) != 0;
  bool reserved3 = (first_byte & kReserved3Bit) != 0;
  int op_code = first_byte & kOpCodeMask;
  bool masked = (second_byte & kMaskBit) != 0;
  *compressed = reserved1;
  if (!final || reserved2 || reserved3)
    return FRAME_ERROR;  // Only compression extension is supported.

  bool closed = false;
  switch (op_code) {
    case kOpCodeClose:
      closed = true;
      break;
    case kOpCodeText:
      break;
    case kOpCodeBinary:        // We don't support binary frames yet.
    case kOpCodeContinuation:  // We don't support binary frames yet.
    case kOpCodePing:          // We don't support binary frames yet.
    case kOpCodePong:          // We don't support binary frames yet.
    default:
      return FRAME_ERROR;
  }

  // In Hybi-17 spec client MUST mask its frame.
  if (client_frame && !masked) {
    return FRAME_ERROR;
  }

  uint64_t payload_length64 = second_byte & kPayloadLengthMask;
  if (payload_length64 > kMaxSingleBytePayloadLength) {
    int extended_payload_length_size;
    if (payload_length64 == kTwoBytePayloadLengthField) {
      extended_payload_length_size = 2;
    } else if (payload_length64 == kEightBytePayloadLengthField) {
      extended_payload_length_size = 8;
    } else {
      return FRAME_ERROR;
    }
    if ((buffer.end() - it) < extended_payload_length_size)
      return FRAME_INCOMPLETE;
    payload_length64 = 0;
    for (int i = 0; i < extended_payload_length_size; ++i) {
      payload_length64 <<= 8;
      payload_length64 |= static_cast<unsigned char>(*it++);
    }
  }

  static const uint64_t max_payload_length = 0x7FFFFFFFFFFFFFFFull;
  static const size_t max_length = SIZE_MAX;
  if (payload_length64 > max_payload_length ||
      payload_length64 > max_length - kMaskingKeyWidthInBytes) {
    // WebSocket frame length too large.
    return FRAME_ERROR;
  }
  size_t payload_length = static_cast<size_t>(payload_length64);

  if (buffer.size() - kMaskingKeyWidthInBytes < payload_length)
    return FRAME_INCOMPLETE;

  std::vector<char>::const_iterator masking_key = it;
  std::vector<char>::const_iterator payload = it + kMaskingKeyWidthInBytes;
  for (size_t i = 0; i < payload_length; ++i)  // Unmask the payload.
    output->insert(output->end(),
                   payload[i] ^ masking_key[i % kMaskingKeyWidthInBytes]);

  size_t pos = it + kMaskingKeyWidthInBytes + payload_length - buffer.begin();
  *bytes_consumed = pos;
  return closed ? FRAME_CLOSE : FRAME_OK;
}

// WS protocol
class WsHandler : public ProtocolHandler {
 public:
  WsHandler(InspectorSocket* inspector, TcpHolder::Pointer tcp)
            : ProtocolHandler(inspector, std::move(tcp)),
              OnCloseSent(&WsHandler::WaitForCloseReply),
              OnCloseRecieved(&WsHandler::CloseFrameReceived),
              dispose_(false) { }

  void AcceptUpgrade(const std::string& accept_key) override { }
  void CancelHandshake() override {}

  void OnEof() override {
    tcp_.reset();
    if (dispose_)
      delete this;
  }

  void OnData(std::vector<char>* data) override {
    // 1. Parse.
    size_t processed = 0;
    do {
      processed = ParseWsFrames(*data);
      // 2. Fix the data size & length
      if (processed > 0) {
        remove_from_beginning(data, processed);
      }
    } while (processed > 0 && !data->empty());
  }

  void Write(const std::vector<char> data) override {
    std::vector<char> output = encode_frame_hybi17(data);
    WriteRaw(output/*, WriteRequest::Cleanup*/);
  }

 protected:
  void Shutdown() override {
    if (tcp_) {
      dispose_ = true;
      SendClose();
    } else {
      delete this;
    }
  }

 private:
  using Callback = void (WsHandler::*)(void);

  static void OnCloseFrameWritten(/*uv_write_t* req, */int status) {
    /*WriteRequest* wr = WriteRequest::from_write_req(req);
    WsHandler* handler = static_cast<WsHandler*>(wr->handler);
    delete wr;*/
    /*Callback cb = handler->OnCloseSent;
    (handler->*cb)();*/
  }

  void WaitForCloseReply() {
    OnCloseRecieved = &WsHandler::OnEof;
  }

  void SendClose() {
    WriteRaw(std::vector<char>(CLOSE_FRAME, CLOSE_FRAME + sizeof(CLOSE_FRAME))/*,
             OnCloseFrameWritten*/);
  }

  void CloseFrameReceived() {
    OnCloseSent = &WsHandler::OnEof;
    SendClose();
  }

  size_t ParseWsFrames(const std::vector<char>& buffer) {
    size_t bytes_consumed = 0;
    std::vector<char> output;
    bool compressed = false;

    ws_decode_result r =  decode_frame_hybi17(buffer,
                                              true /* client_frame */,
                                              &bytes_consumed, &output,
                                              &compressed);
    // Compressed frame means client is ignoring the headers and misbehaves
    if (compressed || r == FRAME_ERROR) {
      OnEof();
      bytes_consumed = 0;
    } else if (r == FRAME_CLOSE) {
      (this->*OnCloseRecieved)();
      bytes_consumed = 0;
    } else if (r == FRAME_OK) {
      delegate()->OnWsFrame(output);
    }
    return bytes_consumed;
  }


  Callback OnCloseSent;
  Callback OnCloseRecieved;
  bool dispose_;
};

// HTTP protocol
class HttpEvent {
 public:
  HttpEvent(const std::string& path, bool upgrade, bool isGET,
            const std::string& ws_key, const std::string& host)
            : path(path), upgrade(upgrade), isGET(isGET), ws_key(ws_key),
              host(host) { }

  std::string path;
  bool upgrade;
  bool isGET;
  std::string ws_key;
  std::string host;
};

class HttpHandler : public ProtocolHandler {
 public:
  explicit HttpHandler(InspectorSocket* inspector, TcpHolder::Pointer tcp)
                       : ProtocolHandler(inspector, std::move(tcp)),
                         parsing_value_(false) {
    llhttp_init(&parser_, HTTP_REQUEST, &parser_settings);
    llhttp_settings_init(&parser_settings);
    parser_settings.on_header_field = OnHeaderField;
    parser_settings.on_header_value = OnHeaderValue;
    parser_settings.on_message_complete = OnMessageComplete;
    parser_settings.on_url = OnPath;
  }

  void AcceptUpgrade(const std::string& accept_key) override {
    char accept_string[ACCEPT_KEY_LENGTH];
    generate_accept_string(accept_key, &accept_string);
    const char accept_ws_prefix[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: ";
    const char accept_ws_suffix[] = "\r\n\r\n";
    std::vector<char> reply(accept_ws_prefix,
                            accept_ws_prefix + sizeof(accept_ws_prefix) - 1);
    reply.insert(reply.end(), accept_string,
                 accept_string + sizeof(accept_string));
    reply.insert(reply.end(), accept_ws_suffix,
                 accept_ws_suffix + sizeof(accept_ws_suffix) - 1);
    if (WriteRaw(reply/*, WriteRequest::Cleanup*/) >= 0) {
      inspector_->SwitchProtocol(new WsHandler(inspector_, std::move(tcp_)));
    } else {
      tcp_.reset();
    }
  }

  void CancelHandshake() override {
    const char HANDSHAKE_FAILED_RESPONSE[] =
        "HTTP/1.0 400 Bad Request\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n\r\n"
        "WebSockets request was expected\r\n";
    WriteRaw(std::vector<char>(HANDSHAKE_FAILED_RESPONSE,
             HANDSHAKE_FAILED_RESPONSE + sizeof(HANDSHAKE_FAILED_RESPONSE) - 1)/*,
             ThenCloseAndReportFailure*/);
  }


  void OnEof() override {
    tcp_.reset();
  }

  void OnData(std::vector<char>* data) override {
    parser_errno_t err;
    err = llhttp_execute(&parser_, data->data(), data->size());

    if (err == HPE_PAUSED_UPGRADE) {
      err = HPE_OK;
      llhttp_resume_after_upgrade(&parser_);
    }
    data->clear();
    if (err != HPE_OK) {
      CancelHandshake();
    }
    // Event handling may delete *this
    std::vector<HttpEvent> events;
    std::swap(events, events_);
    for (const HttpEvent& event : events) {
      if (!IsAllowedHost(event.host) || !event.isGET) {
        CancelHandshake();
        return;
      } else if (!event.upgrade) {
        delegate()->OnHttpGet(event.host, event.path);
      } else if (event.ws_key.empty()) {
        CancelHandshake();
        return;
      } else {
        delegate()->OnSocketUpgrade(event.host, event.path, event.ws_key);
      }
    }
  }

  void Write(const std::vector<char> data) override {
    WriteRaw(data/*, WriteRequest::Cleanup*/);
  }

 protected:
  void Shutdown() override {
    delete this;
  }

 private:
  static void ThenCloseAndReportFailure(/*uv_write_t* req, */int status) {
    /*ProtocolHandler* handler = WriteRequest::from_write_req(req)->handler;
    WriteRequest::Cleanup(req, status);
    handler->inspector()->SwitchProtocol(nullptr);*/
  }

  static int OnHeaderValue(parser_t* parser, const char* at, size_t length) {
    HttpHandler* handler = From(parser);
    handler->parsing_value_ = true;
    handler->headers_[handler->current_header_].append(at, length);
    return 0;
  }

  static int OnHeaderField(parser_t* parser, const char* at, size_t length) {
    HttpHandler* handler = From(parser);
    if (handler->parsing_value_) {
      handler->parsing_value_ = false;
      handler->current_header_.clear();
    }
    handler->current_header_.append(at, length);
    return 0;
  }

  static int OnPath(parser_t* parser, const char* at, size_t length) {
    HttpHandler* handler = From(parser);
    handler->path_.append(at, length);
    return 0;
  }

  static HttpHandler* From(parser_t* parser) {
    return ContainerOf(&HttpHandler::parser_, parser);
  }

  static int OnMessageComplete(parser_t* parser) {
    // Event needs to be fired after the parser is done.
    HttpHandler* handler = From(parser);
    handler->events_.push_back(
        HttpEvent(handler->path_, parser->upgrade, parser->method == HTTP_GET,
                  handler->HeaderValue("Sec-WebSocket-Key"),
                  handler->HeaderValue("Host")));
    handler->path_ = "";
    handler->parsing_value_ = false;
    handler->headers_.clear();
    handler->current_header_ = "";
    return 0;
  }

  std::string HeaderValue(const std::string& header) const {
    bool header_found = false;
    std::string value;
    for (const auto& header_value : headers_) {
      if (utils::StringEqualNoCaseN(header_value.first.data(), header.data(),
                                   header.length())) {
        if (header_found)
          return "";
        value = header_value.second;
        header_found = true;
      }
    }
    return value;
  }

  bool IsAllowedHost(const std::string& host_with_port) const {
    std::string host = TrimPort(host_with_port);
    return host.empty() || IsIPAddress(host)
           || utils::StringEqualNoCase(host.data(), "localhost")
           || utils::StringEqualNoCase(host.data(), "localhost6");
  }

  bool parsing_value_;
  parser_t parser_;
  parser_settings_t parser_settings;
  std::vector<HttpEvent> events_;
  std::string current_header_;
  std::map<std::string, std::string> headers_;
  std::string path_;
};

}  // namespace

// Any protocol
ProtocolHandler::ProtocolHandler(InspectorSocket* inspector,
                                 TcpHolder::Pointer tcp)
                                 : inspector_(inspector), tcp_(std::move(tcp)) {
  CHECK_NOT_NULL(tcp_);
  tcp_->SetHandler(this);
}

int ProtocolHandler::WriteRaw(const std::vector<char>& buffer/*,
                              uv_write_cb write_cb*/) {
  return tcp_->WriteRaw(buffer/*, write_cb*/);
}

InspectorSocket::Delegate* ProtocolHandler::delegate() {
  return tcp_->delegate();
}

std::string ProtocolHandler::GetHost() const {
  // TBD
  return "";
}

// RAII uv_tcp_t wrapper
TcpHolder::TcpHolder(std::shared_ptr<tcp_connection> connection, InspectorSocket::DelegatePointer delegate)
                     : delegate_(std::move(delegate)),
                       connection_(connection), handler_(nullptr) { }

/*static*/ void TcpHolder::OnClosedCallback(void*data) {
  TcpHolder* holder = reinterpret_cast<TcpHolder*>(data);
  delete holder;
}

// static
TcpHolder::Pointer TcpHolder::Accept(tcp_connection::pointer socket, InspectorSocket::DelegatePointer delegate) {
  TcpHolder* tcp = new TcpHolder(socket, std::move(delegate));

  socket->registerCloseCallback(TcpHolder::OnClosedCallback, tcp);
  socket->registerReadCallback(TcpHolder::OnDataReceivedCb, tcp);
  socket->read_loop_async();
  
  return TcpHolder::Pointer(tcp);

}

void TcpHolder::read_loop() {
  connection_->read_loop_async();
}

void TcpHolder::SetHandler(ProtocolHandler* handler) {
  handler_ = handler;
}

int TcpHolder::WriteRaw(const std::vector<char>& buffer/*, uv_write_cb write_cb*/) {
#if DUMP_WRITES
  printf("%s (%ld bytes):\n", __FUNCTION__, buffer.size());
  dump_hex(buffer.data(), buffer.size());
  printf("\n");
#endif

  // Freed in write_request_cleanup
  /*WriteRequest* wr = new WriteRequest(handler_, buffer);
  uv_stream_t* stream = reinterpret_cast<uv_stream_t*>(&tcp_);
  int err = uv_write(&wr->req, stream, &wr->buf, 1, write_cb);
  if (err < 0)
    delete wr;
  return err < 0;*/
  connection_->write_async(buffer);
  return 0;
}

InspectorSocket::Delegate* TcpHolder::delegate() {
  return delegate_.get();
}

void TcpHolder::OnDataReceivedCb(std::vector<char>& wiredata, bool iseof, void*data) {
  TcpHolder* holder = reinterpret_cast<TcpHolder*>(data);
  
  if (iseof) {
    holder->handler_->OnEof();
  }
  else {
    holder->handler_->OnData(&wiredata);
  }
}

/*static*/ void TcpHolder::DisconnectAndDispose(TcpHolder* holder) {
  holder->connection_->close();
}

InspectorSocket::~InspectorSocket() = default;

// static
void InspectorSocket::Shutdown(ProtocolHandler* handler) {
  handler->Shutdown();
}

// static
InspectorSocket::Pointer InspectorSocket::Accept(std::shared_ptr<tcp_connection> connection, DelegatePointer delegate) {
  auto tcp = TcpHolder::Accept(connection, std::move(delegate));
  if (tcp) {
    InspectorSocket* inspector = new InspectorSocket();
    inspector->SwitchProtocol(new HttpHandler(inspector, std::move(tcp)));
    return InspectorSocket::Pointer(inspector);
  } else {
    return InspectorSocket::Pointer(nullptr);
  }
}

void InspectorSocket::AcceptUpgrade(const std::string& ws_key) {
  protocol_handler_->AcceptUpgrade(ws_key);
}

void InspectorSocket::CancelHandshake() {
  protocol_handler_->CancelHandshake();
}

std::string InspectorSocket::GetHost() {
  return protocol_handler_->GetHost();
}

void InspectorSocket::SwitchProtocol(ProtocolHandler* handler) {
  protocol_handler_.reset(std::move(handler));
}

void InspectorSocket::Write(const char* data, size_t len) {
  protocol_handler_->Write(std::vector<char>(data, data + len));
}

}  // namespace inspector
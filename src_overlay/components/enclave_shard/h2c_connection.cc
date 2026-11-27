// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/h2c_connection.h"

#include <algorithm>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "build/build_config.h"
#include "net/base/address_list.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/stream_socket.h"
#include "net/socket/tcp_client_socket.h"
#include "net/third_party/quiche/src/quiche/http2/adapter/oghttp2_adapter.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

#if BUILDFLAG(IS_POSIX)
#include "net/socket/unix_domain_client_socket_posix.h"
#endif

namespace enclave::shard {

namespace {

using http2::adapter::Header;
using http2::adapter::Http2ErrorCode;
using http2::adapter::Http2StreamId;
using http2::adapter::OgHttp2Adapter;

constexpr int kReadSize = 64 * 1024;

// Only to the local Shard daemon, never to the network.
constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("enclave_shard_connector", R"(
      semantics {
        sender: "Enclave Shard connector"
        description:
          "Talks to the Shard program running on this computer, over a "
          "unix socket or 127.0.0.1, to keep browser data in the user's "
          "Shard vault."
        trigger: "Sync via Shard is on."
        data: "Sync entities and logins, sent to the local daemon only."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Settings > Sync via Shard."
      })");

Header MakeHeader(std::string_view key, std::string_view value) {
  return {std::string(key), std::string(value)};
}

// gRPC over HTTP/2: HTTP status when no grpc-status came.
Code CodeForHttpStatus(int http_status) {
  switch (http_status) {
    case 400:
      return Code::kInternal;
    case 401:
      return Code::kUnauthenticated;
    case 403:
      return Code::kPermissionDenied;
    case 404:
      return Code::kUnimplemented;
    case 429:
    case 502:
    case 503:
    case 504:
      return Code::kUnavailable;
    default:
      return Code::kUnknown;
  }
}

}  // namespace

H2cConnection::Stream::Stream() = default;
H2cConnection::Stream::~Stream() = default;

H2cConnection::H2cConnection() {
  Init(Perspective::kClient);
}

H2cConnection::H2cConnection(std::unique_ptr<net::StreamSocket> socket,
                             ServerHandler* handler)
    : socket_(std::move(socket)), handler_(handler) {
  Init(Perspective::kServer);
  state_ = State::kOpen;
  Flush();  // Server preface (SETTINGS).
  DoRead();
}

H2cConnection::~H2cConnection() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void H2cConnection::Init(Perspective perspective) {
  perspective_ = perspective;
  OgHttp2Adapter::Options options;
  options.perspective = perspective == Perspective::kClient
                            ? http2::adapter::Perspective::kClient
                            : http2::adapter::Perspective::kServer;
  adapter_ = OgHttp2Adapter::Create(*this, options);
  read_buffer_ = base::MakeRefCounted<net::IOBufferWithSize>(kReadSize);
}

void H2cConnection::Connect(const ConnectorAddress& address,
                            base::OnceCallback<void(int)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK_EQ(perspective_, Perspective::kClient);
  CHECK_EQ(state_, State::kIdle);
  connect_callback_ = std::move(callback);
  state_ = State::kConnecting;

  if (address.kind == ConnectorAddress::Kind::kUnix) {
#if BUILDFLAG(IS_POSIX)
    socket_ = std::make_unique<net::UnixDomainClientSocket>(
        address.unix_path, /*use_abstract_namespace=*/false);
#endif
  } else {
    net::IPAddress ip;
    if (ip.AssignFromIPLiteral(address.host) && ip.IsLoopback()) {
      socket_ = std::make_unique<net::TCPClientSocket>(
          net::AddressList(net::IPEndPoint(ip, address.port)),
          /*socket_performance_watcher=*/nullptr,
          /*network_quality_estimator=*/nullptr, /*net_log=*/nullptr,
          net::NetLogSource(), net::handles::kInvalidNetworkHandle);
    }
  }
  if (!socket_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&H2cConnection::OnConnected,
                                  weak_factory_.GetWeakPtr(),
                                  net::ERR_ADDRESS_INVALID));
    return;
  }
  const int result = socket_->Connect(base::BindOnce(
      &H2cConnection::OnConnected, weak_factory_.GetWeakPtr()));
  if (result != net::ERR_IO_PENDING) {
    // Callbacks never run synchronously (ConnectorTransport contract).
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&H2cConnection::OnConnected,
                                  weak_factory_.GetWeakPtr(), result));
  }
}

void H2cConnection::OnConnected(int result) {
  if (result != net::OK) {
    state_ = State::kClosed;
    socket_.reset();
    std::move(connect_callback_).Run(result);
    return;
  }
  state_ = State::kOpen;
  Flush();  // Client preface.
  DoRead();
  std::move(connect_callback_).Run(net::OK);
}

int32_t H2cConnection::StartCall(std::string_view path,
                                 const Metadata& metadata,
                                 std::string_view request,
                                 CallObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK_EQ(perspective_, Perspective::kClient);
  if (state_ != State::kOpen) {
    return -1;
  }
  std::vector<Header> headers = {
      MakeHeader(":method", "POST"),
      MakeHeader(":scheme", "http"),
      MakeHeader(":authority", "localhost"),
      MakeHeader(":path", path),
      MakeHeader("content-type", "application/grpc"),
      MakeHeader("te", "trailers"),
      MakeHeader("user-agent", "enclave-shard/1"),
  };
  for (const auto& [key, value] : metadata) {
    headers.push_back(MakeHeader(key, value));
  }
  const int32_t stream_id =
      adapter_->SubmitRequest(headers, /*end_stream=*/false, nullptr);
  if (stream_id < 0) {
    return -1;
  }
  Stream& stream = streams_[stream_id];
  stream.observer = observer;
  stream.body = EncodeGrpcMessage(request);
  stream.body_done = true;
  if (!processing_) {
    Flush();
  }
  return stream_id;
}

void H2cConnection::CancelCall(int32_t stream_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return;
  }
  streams_.erase(it);
  if (state_ == State::kOpen) {
    adapter_->SubmitRst(stream_id, Http2ErrorCode::CANCEL);
    if (!processing_) {
      Flush();
    }
  }
}

void H2cConnection::SendMessage(int32_t stream_id, std::string_view message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK_EQ(perspective_, Perspective::kServer);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || state_ != State::kOpen) {
    return;
  }
  Stream& stream = it->second;
  if (stream.header_blocks == 0) {
    // header_blocks counts our response headers on the server side.
    stream.header_blocks = 1;
    adapter_->SubmitResponse(
        stream_id,
        {MakeHeader(":status", "200"),
         MakeHeader("content-type", "application/grpc")},
        /*end_stream=*/false);
  }
  stream.body += EncodeGrpcMessage(message);
  adapter_->ResumeStream(stream_id);
  if (!processing_) {
    Flush();
  }
}

void H2cConnection::Finish(int32_t stream_id, const Status& status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK_EQ(perspective_, Perspective::kServer);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || state_ != State::kOpen) {
    return;
  }
  Stream& stream = it->second;
  std::vector<Header> trailers = {
      MakeHeader("grpc-status",
                 base::NumberToString(static_cast<int>(status.code)))};
  if (!status.message.empty()) {
    trailers.push_back(MakeHeader(
        "grpc-message", PercentEncodeGrpcMessage(status.message)));
  }
  if (stream.header_blocks == 0) {
    // Trailers-only response.
    trailers.insert(trailers.begin(),
                    {MakeHeader(":status", "200"),
                     MakeHeader("content-type", "application/grpc")});
    adapter_->SubmitResponse(stream_id, trailers, /*end_stream=*/true);
  } else {
    stream.body_done = true;
    adapter_->SubmitTrailer(stream_id, trailers);
    adapter_->ResumeStream(stream_id);
  }
  stream.header_blocks = 2;
  if (!processing_) {
    Flush();
  }
}

// --- socket ------------------------------------------------------------------

void H2cConnection::DoRead() {
  while (state_ == State::kOpen) {
    const int result = socket_->Read(
        read_buffer_.get(), kReadSize,
        base::BindOnce(&H2cConnection::OnRead, weak_factory_.GetWeakPtr()));
    if (result == net::ERR_IO_PENDING) {
      return;
    }
    // Synchronous result: handle it in a fresh task so observers never run
    // inside a caller of this class.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&H2cConnection::OnRead,
                                  weak_factory_.GetWeakPtr(), result));
    return;
  }
}

void H2cConnection::OnRead(int result) {
  if (state_ != State::kOpen) {
    return;
  }
  if (result <= 0) {
    Lose(result == 0 ? "connection closed" : net::ErrorToShortString(result));
    return;
  }

  processing_ = true;
  const int64_t processed = adapter_->ProcessBytes(
      std::string_view(read_buffer_->data(), static_cast<size_t>(result)));
  processing_ = false;
  if (processed < 0 || state_ == State::kClosed) {
    Lose("HTTP/2 error");
    return;
  }
  for (const auto& [stream_id, bytes] : consumed_) {
    adapter_->MarkDataConsumedForStream(stream_id, bytes);
  }
  consumed_.clear();
  Flush();

  base::WeakPtr<H2cConnection> self = weak_factory_.GetWeakPtr();
  DeliverEvents();
  if (self && state_ == State::kOpen) {
    DoRead();
  }
}

void H2cConnection::Flush() {
  if (state_ != State::kOpen) {
    return;
  }
  if (adapter_->want_write() && adapter_->Send() != 0) {
    // Send() fails only after the visitor reported an error.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&H2cConnection::Lose,
                                  weak_factory_.GetWeakPtr(), "send failed"));
    return;
  }
  DoWrite();
  if (!processing_ && !events_.empty()) {
    // A stream can close while sending (our END_STREAM or RST went out).
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&H2cConnection::DeliverEvents,
                                  weak_factory_.GetWeakPtr()));
  }
}

void H2cConnection::DoWrite() {
  while (state_ == State::kOpen && !write_buffer_ && !out_.empty()) {
    const size_t size = out_.size();
    write_buffer_ = base::MakeRefCounted<net::DrainableIOBuffer>(
        base::MakeRefCounted<net::StringIOBuffer>(std::move(out_)), size);
    out_.clear();
    while (write_buffer_ && write_buffer_->BytesRemaining() > 0) {
      const int result = socket_->Write(
          write_buffer_.get(), write_buffer_->BytesRemaining(),
          base::BindOnce(&H2cConnection::OnWrite, weak_factory_.GetWeakPtr()),
          kTrafficAnnotation);
      if (result == net::ERR_IO_PENDING) {
        return;
      }
      if (result <= 0) {
        write_buffer_.reset();
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(&H2cConnection::Lose, weak_factory_.GetWeakPtr(),
                           net::ErrorToShortString(result)));
        return;
      }
      write_buffer_->DidConsume(result);
    }
    write_buffer_.reset();
  }
}

void H2cConnection::OnWrite(int result) {
  if (state_ != State::kOpen) {
    return;
  }
  if (result <= 0) {
    write_buffer_.reset();
    Lose(net::ErrorToShortString(result));
    return;
  }
  write_buffer_->DidConsume(result);
  if (write_buffer_->BytesRemaining() > 0) {
    const int next = socket_->Write(
        write_buffer_.get(), write_buffer_->BytesRemaining(),
        base::BindOnce(&H2cConnection::OnWrite, weak_factory_.GetWeakPtr()),
        kTrafficAnnotation);
    if (next != net::ERR_IO_PENDING) {
      OnWrite(next);
    }
    return;
  }
  write_buffer_.reset();
  DoWrite();
}

void H2cConnection::Lose(std::string reason) {
  if (state_ == State::kClosed && streams_.empty() && !lost_callback_) {
    return;
  }
  state_ = State::kClosed;
  socket_.reset();
  write_buffer_.reset();
  out_.clear();

  for (auto& [stream_id, stream] : streams_) {
    if (perspective_ == Perspective::kClient) {
      events_.push_back({Event::Kind::kClosed, stream_id, {},
                         Status::Transport("Shard connection lost: " +
                                           reason)});
    } else {
      events_.push_back({Event::Kind::kGone, stream_id, {}, {}});
    }
  }

  base::WeakPtr<H2cConnection> self = weak_factory_.GetWeakPtr();
  DeliverEvents();
  if (self && lost_callback_) {
    std::move(lost_callback_).Run();
  }
}

void H2cConnection::DeliverEvents() {
  base::WeakPtr<H2cConnection> self = weak_factory_.GetWeakPtr();
  std::vector<Event> events = std::move(events_);
  events_.clear();
  for (Event& event : events) {
    if (!self) {
      return;
    }
    auto it = streams_.find(event.stream_id);
    if (it == streams_.end()) {
      continue;  // Cancelled meanwhile.
    }
    switch (event.kind) {
      case Event::Kind::kMessage:
        if (it->second.observer) {
          it->second.observer->OnCallMessage(event.stream_id,
                                             std::move(event.message));
        }
        break;
      case Event::Kind::kClosed: {
        CallObserver* observer = it->second.observer;
        streams_.erase(it);
        if (observer) {
          observer->OnCallClosed(event.stream_id, std::move(event.status));
        }
        break;
      }
      case Event::Kind::kRequest:
        handler_->OnRequest(event.stream_id, it->second.path,
                            it->second.metadata, std::move(event.message));
        break;
      case Event::Kind::kGone:
        streams_.erase(it);
        handler_->OnStreamGone(event.stream_id);
        break;
    }
  }
}

Status H2cConnection::StatusOf(const Stream& stream,
                               Http2ErrorCode error_code) const {
  if (stream.broken) {
    return {Code::kInternal, "bad gRPC message from Shard"};
  }
  if (stream.grpc_status) {
    return Status::FromWire(*stream.grpc_status, stream.grpc_message);
  }
  if (error_code != Http2ErrorCode::HTTP2_NO_ERROR) {
    return {Code::kUnavailable,
            "stream reset by Shard: " +
                base::NumberToString(static_cast<int>(error_code))};
  }
  if (stream.http_status != 200) {
    return {CodeForHttpStatus(stream.http_status),
            "HTTP " + base::NumberToString(stream.http_status)};
  }
  return {Code::kInternal, "no grpc-status from Shard"};
}

// --- Http2VisitorInterface -----------------------------------------------------

int64_t H2cConnection::OnReadyToSend(absl::string_view serialized) {
  if (state_ != State::kOpen) {
    return kSendError;
  }
  out_.append(serialized);
  return static_cast<int64_t>(serialized.size());
}

H2cConnection::DataFrameHeaderInfo H2cConnection::OnReadyToSendDataForStream(
    Http2StreamId stream_id,
    size_t max_length) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    // Cancelled: end the body; the RST follows.
    return {0, /*end_data=*/true, /*end_stream=*/true};
  }
  const Stream& stream = it->second;
  const size_t length = std::min(max_length, stream.body.size());
  const bool end_data = stream.body_done && length == stream.body.size();
  // The client ends its stream with the request; the server ends it with the
  // trailers.
  const bool end_stream = end_data && perspective_ == Perspective::kClient;
  return {static_cast<int64_t>(length), end_data, end_stream};
}

bool H2cConnection::SendDataFrame(Http2StreamId stream_id,
                                  absl::string_view frame_header,
                                  size_t payload_bytes) {
  auto it = streams_.find(stream_id);
  out_.append(frame_header);
  if (it == streams_.end()) {
    CHECK_EQ(payload_bytes, 0u);
    return true;
  }
  Stream& stream = it->second;
  CHECK_LE(payload_bytes, stream.body.size());
  out_.append(stream.body, 0, payload_bytes);
  stream.body.erase(0, payload_bytes);
  return true;
}

void H2cConnection::OnConnectionError(ConnectionError error) {
  // Handled after ProcessBytes / Send return.
  state_ = State::kClosed;
}

bool H2cConnection::OnBeginHeadersForStream(Http2StreamId stream_id) {
  if (perspective_ == Perspective::kServer) {
    streams_.try_emplace(stream_id);
    return true;
  }
  auto it = streams_.find(stream_id);
  if (it != streams_.end()) {
    ++it->second.header_blocks;
  }
  return true;
}

H2cConnection::OnHeaderResult H2cConnection::OnHeaderForStream(
    Http2StreamId stream_id,
    absl::string_view key,
    absl::string_view value) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return OnHeaderResult::HEADER_OK;
  }
  Stream& stream = it->second;
  if (perspective_ == Perspective::kServer) {
    if (key == ":path") {
      stream.path = std::string(value);
    } else if (!key.starts_with(":")) {
      stream.metadata.emplace_back(std::string(key), std::string(value));
    }
    return OnHeaderResult::HEADER_OK;
  }
  if (key == ":status") {
    base::StringToInt(value, &stream.http_status);
  } else if (key == "grpc-status") {
    stream.grpc_status = ParseGrpcStatus(value);
    if (!stream.grpc_status) {
      stream.grpc_status = static_cast<int>(Code::kUnknown);
    }
  } else if (key == "grpc-message") {
    stream.grpc_message = PercentDecodeGrpcMessage(value);
  }
  return OnHeaderResult::HEADER_OK;
}

bool H2cConnection::OnEndHeadersForStream(Http2StreamId stream_id) {
  return true;
}

bool H2cConnection::OnBeginDataForStream(Http2StreamId stream_id,
                                         size_t payload_length) {
  return true;
}

bool H2cConnection::OnDataPaddingLength(Http2StreamId stream_id,
                                        size_t padding_length) {
  return true;  // The adapter accounts for padding itself.
}

bool H2cConnection::OnDataForStream(Http2StreamId stream_id,
                                    absl::string_view data) {
  consumed_[stream_id] += data.size();
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || it->second.broken) {
    return true;
  }
  Stream& stream = it->second;
  stream.reader.Append(data);
  if (perspective_ == Perspective::kServer) {
    return true;  // The request is complete at OnEndStream.
  }
  std::string message;
  for (;;) {
    switch (stream.reader.Next(&message)) {
      case GrpcMessageReader::Result::kMessage:
        events_.push_back(
            {Event::Kind::kMessage, stream_id, std::move(message), {}});
        message.clear();
        continue;
      case GrpcMessageReader::Result::kNeedMore:
        return true;
      case GrpcMessageReader::Result::kError:
        stream.broken = true;
        adapter_->SubmitRst(stream_id, Http2ErrorCode::PROTOCOL_ERROR);
        return true;
    }
  }
}

bool H2cConnection::OnEndStream(Http2StreamId stream_id) {
  if (perspective_ != Perspective::kServer) {
    return true;
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return true;
  }
  std::string message;
  if (it->second.reader.Next(&message) != GrpcMessageReader::Result::kMessage ||
      it->second.reader.has_partial()) {
    adapter_->SubmitRst(stream_id, Http2ErrorCode::PROTOCOL_ERROR);
    return true;
  }
  events_.push_back({Event::Kind::kRequest, stream_id, std::move(message), {}});
  return true;
}

void H2cConnection::OnRstStream(Http2StreamId stream_id,
                                Http2ErrorCode error_code) {
  // OnCloseStream follows.
}

bool H2cConnection::OnCloseStream(Http2StreamId stream_id,
                                  Http2ErrorCode error_code) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return true;
  }
  if (perspective_ == Perspective::kServer) {
    events_.push_back({Event::Kind::kGone, stream_id, {}, {}});
    return true;
  }
  Stream& stream = it->second;
  if (stream.reader.has_partial()) {
    stream.broken = true;
  }
  events_.push_back(
      {Event::Kind::kClosed, stream_id, {}, StatusOf(stream, error_code)});
  return true;
}

bool H2cConnection::OnGoAway(Http2StreamId last_accepted_stream_id,
                             Http2ErrorCode error_code,
                             absl::string_view opaque_data) {
  // Streams above last_accepted_stream_id are closed by the adapter; the rest
  // finish normally. No new calls: the owner reconnects on the next one.
  return true;
}

int H2cConnection::OnBeforeFrameSent(uint8_t frame_type,
                                     Http2StreamId stream_id,
                                     size_t length,
                                     uint8_t flags) {
  return 0;
}

int H2cConnection::OnFrameSent(uint8_t frame_type,
                               Http2StreamId stream_id,
                               size_t length,
                               uint8_t flags,
                               uint32_t error_code) {
  return 0;
}

bool H2cConnection::OnInvalidFrame(Http2StreamId stream_id,
                                   InvalidFrameError error) {
  return true;
}

bool H2cConnection::OnMetadataForStream(Http2StreamId stream_id,
                                        absl::string_view metadata) {
  return true;
}

bool H2cConnection::OnMetadataEndForStream(Http2StreamId stream_id) {
  return true;
}

}  // namespace enclave::shard

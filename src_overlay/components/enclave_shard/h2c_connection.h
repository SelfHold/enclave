// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_H2C_CONNECTION_H_
#define COMPONENTS_ENCLAVE_SHARD_H2C_CONNECTION_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "components/enclave_shard/connector_address.h"
#include "components/enclave_shard/grpc_frame.h"
#include "components/enclave_shard/status.h"
#include "net/third_party/quiche/src/quiche/http2/adapter/http2_visitor_interface.h"

namespace http2::adapter {
class OgHttp2Adapter;
}

namespace net {
class IOBuffer;
class DrainableIOBuffer;
class StreamSocket;
}  // namespace net

namespace enclave::shard {

// One HTTP/2 connection without TLS ("h2c", prior knowledge) to a local gRPC
// server, as a client or, for tests, as a server. The HTTP/2 work is done by
// quiche's OgHttp2Adapter; this class moves bytes between it and a socket and
// turns streams into gRPC calls.
//
// Lives on a sequence with an IO message pump. Observers are told about
// events only after the adapter has finished with the bytes that caused them,
// so they may start or cancel calls, or delete this object, from inside.
class H2cConnection : public http2::adapter::Http2VisitorInterface {
 public:
  using Metadata = std::vector<std::pair<std::string, std::string>>;

  // One gRPC call seen from the client.
  class CallObserver {
   public:
    virtual void OnCallMessage(int32_t stream_id, std::string message) = 0;
    // Last event of a call. Not sent for calls cancelled with CancelCall.
    virtual void OnCallClosed(int32_t stream_id, Status status) = 0;

   protected:
    virtual ~CallObserver() = default;
  };

  // Server side, tests only: one request per stream (unary or the start of
  // a server stream). Answer with SendMessage / Finish.
  class ServerHandler {
   public:
    virtual void OnRequest(int32_t stream_id,
                           const std::string& path,
                           const Metadata& metadata,
                           std::string message) = 0;
    // The client went away or reset the stream.
    virtual void OnStreamGone(int32_t stream_id) {}

   protected:
    virtual ~ServerHandler() = default;
  };

  enum class Perspective { kClient, kServer };

  // Client.
  H2cConnection();
  // Server over an accepted socket (tests).
  H2cConnection(std::unique_ptr<net::StreamSocket> socket,
                ServerHandler* handler);
  H2cConnection(const H2cConnection&) = delete;
  H2cConnection& operator=(const H2cConnection&) = delete;
  ~H2cConnection() override;

  // Client: connects and sends the preface. `callback` gets a net error code.
  void Connect(const ConnectorAddress& address,
               base::OnceCallback<void(int)> callback);

  // Called once when the connection is lost (any open call is closed with a
  // transport error first). Not called when this object is deleted.
  void set_lost_callback(base::OnceClosure callback) {
    lost_callback_ = std::move(callback);
  }

  bool is_open() const { return state_ == State::kOpen; }

  // Client: starts a call of `path` (e.g. "/pkg.Service/Method") with one
  // request message. Returns the stream id, or -1 when the connection is not
  // open (no callback then). `observer` must outlive the call.
  int32_t StartCall(std::string_view path,
                    const Metadata& metadata,
                    std::string_view request,
                    CallObserver* observer);
  // Resets the stream; the observer hears nothing more about it.
  void CancelCall(int32_t stream_id);

  // Server.
  void SendMessage(int32_t stream_id, std::string_view message);
  void Finish(int32_t stream_id, const Status& status);

  // Http2VisitorInterface:
  int64_t OnReadyToSend(absl::string_view serialized) override;
  DataFrameHeaderInfo OnReadyToSendDataForStream(
      http2::adapter::Http2StreamId stream_id,
      size_t max_length) override;
  bool SendDataFrame(http2::adapter::Http2StreamId stream_id,
                     absl::string_view frame_header,
                     size_t payload_bytes) override;
  void OnConnectionError(ConnectionError error) override;
  void OnSettingsStart() override {}
  void OnSetting(http2::adapter::Http2Setting setting) override {}
  void OnSettingsEnd() override {}
  void OnSettingsAck() override {}
  bool OnBeginHeadersForStream(
      http2::adapter::Http2StreamId stream_id) override;
  OnHeaderResult OnHeaderForStream(http2::adapter::Http2StreamId stream_id,
                                   absl::string_view key,
                                   absl::string_view value) override;
  bool OnEndHeadersForStream(http2::adapter::Http2StreamId stream_id) override;
  bool OnBeginDataForStream(http2::adapter::Http2StreamId stream_id,
                            size_t payload_length) override;
  bool OnDataPaddingLength(http2::adapter::Http2StreamId stream_id,
                           size_t padding_length) override;
  bool OnDataForStream(http2::adapter::Http2StreamId stream_id,
                       absl::string_view data) override;
  bool OnEndStream(http2::adapter::Http2StreamId stream_id) override;
  void OnRstStream(http2::adapter::Http2StreamId stream_id,
                   http2::adapter::Http2ErrorCode error_code) override;
  bool OnCloseStream(http2::adapter::Http2StreamId stream_id,
                     http2::adapter::Http2ErrorCode error_code) override;
  void OnPriorityForStream(http2::adapter::Http2StreamId stream_id,
                           http2::adapter::Http2StreamId parent_stream_id,
                           int weight,
                           bool exclusive) override {}
  void OnPing(http2::adapter::Http2PingId ping_id, bool is_ack) override {}
  void OnPushPromiseForStream(
      http2::adapter::Http2StreamId stream_id,
      http2::adapter::Http2StreamId promised_stream_id) override {}
  bool OnGoAway(http2::adapter::Http2StreamId last_accepted_stream_id,
                http2::adapter::Http2ErrorCode error_code,
                absl::string_view opaque_data) override;
  void OnWindowUpdate(http2::adapter::Http2StreamId stream_id,
                      int window_increment) override {}
  int OnBeforeFrameSent(uint8_t frame_type,
                        http2::adapter::Http2StreamId stream_id,
                        size_t length,
                        uint8_t flags) override;
  int OnFrameSent(uint8_t frame_type,
                  http2::adapter::Http2StreamId stream_id,
                  size_t length,
                  uint8_t flags,
                  uint32_t error_code) override;
  bool OnInvalidFrame(http2::adapter::Http2StreamId stream_id,
                      InvalidFrameError error) override;
  void OnBeginMetadataForStream(http2::adapter::Http2StreamId stream_id,
                                size_t payload_length) override {}
  bool OnMetadataForStream(http2::adapter::Http2StreamId stream_id,
                           absl::string_view metadata) override;
  bool OnMetadataEndForStream(
      http2::adapter::Http2StreamId stream_id) override;
  void OnErrorDebug(absl::string_view message) override {}

 private:
  enum class State { kIdle, kConnecting, kOpen, kClosed };

  struct Stream {
    Stream();
    ~Stream();

    raw_ptr<CallObserver> observer = nullptr;  // Client.
    // Outgoing body bytes not yet handed to the adapter.
    std::string body;
    // Server: Finish() was called, no more body after `body`.
    bool body_done = false;
    int header_blocks = 0;
    int http_status = 0;  // Client.
    std::optional<int> grpc_status;
    std::string grpc_message;
    std::string path;  // Server.
    Metadata metadata;  // Server.
    GrpcMessageReader reader;
    bool broken = false;  // A bad message arrived; the stream is reset.
  };

  // Something an observer must hear about, delivered after ProcessBytes.
  struct Event {
    enum class Kind { kMessage, kClosed, kRequest, kGone };
    Kind kind;
    int32_t stream_id;
    std::string message;
    Status status;
  };

  void Init(Perspective perspective);
  void OnConnected(int result);
  void DoRead();
  void OnRead(int result);
  void Flush();
  void DoWrite();
  void OnWrite(int result);
  // Closes every stream with a transport error and tells the owner.
  void Lose(std::string reason);
  void DeliverEvents();
  Status StatusOf(const Stream& stream,
                  http2::adapter::Http2ErrorCode error_code) const;

  Perspective perspective_ = Perspective::kClient;
  State state_ = State::kIdle;
  std::unique_ptr<net::StreamSocket> socket_;
  std::unique_ptr<http2::adapter::OgHttp2Adapter> adapter_;
  raw_ptr<ServerHandler> handler_ = nullptr;
  base::OnceCallback<void(int)> connect_callback_;
  base::OnceClosure lost_callback_;

  std::map<int32_t, Stream> streams_;
  std::vector<Event> events_;
  // Received body bytes to acknowledge to the adapter (flow control).
  std::map<int32_t, size_t> consumed_;
  bool processing_ = false;

  std::string out_;  // Serialized frames waiting for the socket.
  scoped_refptr<net::DrainableIOBuffer> write_buffer_;
  scoped_refptr<net::IOBuffer> read_buffer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<H2cConnection> weak_factory_{this};
};

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_H2C_CONNECTION_H_

// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/bezel_cloudsync/cross_thread_transport.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"

namespace bezel::cloudsync {

// Owns the inner transport and its watches; lives on the IO sequence.
class CrossThreadTransport::IoSide {
 public:
  explicit IoSide(Factory factory) : inner_(std::move(factory).Run()) {}

  void Call(std::string method,
            std::string request,
            base::TimeDelta timeout,
            UnaryCallback callback) {
    inner_->Call(method, std::move(request), timeout, std::move(callback));
  }

  void Watch(int id,
             base::RepeatingCallback<void(std::string)> on_message,
             base::OnceCallback<void(Status)> on_end) {
    auto relay = std::make_unique<Relay>(std::move(on_message),
                                         std::move(on_end));
    Relay* raw = relay.get();
    relay->handle = inner_->Watch(raw);
    relays_[id] = std::move(relay);
  }

  void StopWatch(int id) { relays_.erase(id); }

  void SetToken(std::string token) { inner_->SetToken(std::move(token)); }

 private:
  struct Relay : public WatchObserver {
    Relay(base::RepeatingCallback<void(std::string)> on_message,
          base::OnceCallback<void(Status)> on_end)
        : on_message(std::move(on_message)), on_end(std::move(on_end)) {}
    ~Relay() override { handle.reset(); }

    void OnWatchMessage(std::string change) override {
      on_message.Run(std::move(change));
    }
    void OnWatchEnd(Status status) override {
      if (on_end) {
        std::move(on_end).Run(std::move(status));
      }
    }

    base::RepeatingCallback<void(std::string)> on_message;
    base::OnceCallback<void(Status)> on_end;
    std::unique_ptr<WatchHandle> handle;
  };

  std::unique_ptr<ConnectorTransport> inner_;
  std::map<int, std::unique_ptr<Relay>> relays_;
};

class CrossThreadTransport::Handle : public WatchHandle {
 public:
  Handle(base::WeakPtr<CrossThreadTransport> transport, int id)
      : transport_(std::move(transport)), id_(id) {}
  ~Handle() override {
    if (transport_) {
      transport_->StopWatch(id_);
    }
  }

 private:
  base::WeakPtr<CrossThreadTransport> transport_;
  const int id_;
};

CrossThreadTransport::CrossThreadTransport(
    scoped_refptr<base::SequencedTaskRunner> io_task_runner,
    Factory factory)
    : io_side_(std::move(io_task_runner), std::move(factory)) {}

CrossThreadTransport::~CrossThreadTransport() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void CrossThreadTransport::Call(std::string_view method,
                                std::string request,
                                base::TimeDelta timeout,
                                UnaryCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  io_side_.AsyncCall(&IoSide::Call)
      .WithArgs(std::string(method), std::move(request), timeout,
                base::BindPostTaskToCurrentDefault(std::move(callback)));
}

std::unique_ptr<ConnectorTransport::WatchHandle> CrossThreadTransport::Watch(
    WatchObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const int id = next_watch_id_++;
  watches_[id] = observer;
  io_side_.AsyncCall(&IoSide::Watch)
      .WithArgs(id,
                base::BindPostTaskToCurrentDefault(base::BindRepeating(
                    &CrossThreadTransport::OnWatchMessage,
                    weak_factory_.GetWeakPtr(), id)),
                base::BindPostTaskToCurrentDefault(
                    base::BindOnce(&CrossThreadTransport::OnWatchEnd,
                                   weak_factory_.GetWeakPtr(), id)));
  return std::make_unique<Handle>(weak_factory_.GetWeakPtr(), id);
}

void CrossThreadTransport::SetToken(std::string token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  io_side_.AsyncCall(&IoSide::SetToken).WithArgs(std::move(token));
}

void CrossThreadTransport::OnWatchMessage(int id, std::string change) {
  auto it = watches_.find(id);
  if (it != watches_.end()) {
    it->second->OnWatchMessage(std::move(change));
  }
}

void CrossThreadTransport::OnWatchEnd(int id, Status status) {
  auto it = watches_.find(id);
  if (it == watches_.end()) {
    return;
  }
  WatchObserver* observer = it->second;
  watches_.erase(it);
  io_side_.AsyncCall(&IoSide::StopWatch).WithArgs(id);
  observer->OnWatchEnd(std::move(status));
}

void CrossThreadTransport::StopWatch(int id) {
  if (watches_.erase(id)) {
    io_side_.AsyncCall(&IoSide::StopWatch).WithArgs(id);
  }
}

}  // namespace bezel::cloudsync

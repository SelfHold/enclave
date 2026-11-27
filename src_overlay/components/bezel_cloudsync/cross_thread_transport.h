// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_CROSS_THREAD_TRANSPORT_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_CROSS_THREAD_TRANSPORT_H_

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "components/bezel_cloudsync/connector_transport.h"

namespace bezel::cloudsync {

// Runs another transport on a different sequence (desktop: GrpcTransport on
// the browser's IO thread, which has the IO message pump sockets need) and
// is used from this one (the UI thread).
class CrossThreadTransport : public ConnectorTransport {
 public:
  // Runs on `io_task_runner` and makes the inner transport there.
  using Factory = base::OnceCallback<std::unique_ptr<ConnectorTransport>()>;

  CrossThreadTransport(scoped_refptr<base::SequencedTaskRunner> io_task_runner,
                       Factory factory);
  CrossThreadTransport(const CrossThreadTransport&) = delete;
  CrossThreadTransport& operator=(const CrossThreadTransport&) = delete;
  ~CrossThreadTransport() override;

  // ConnectorTransport:
  void Call(std::string_view method,
            std::string request,
            base::TimeDelta timeout,
            UnaryCallback callback) override;
  std::unique_ptr<WatchHandle> Watch(WatchObserver* observer) override;
  void SetToken(std::string token) override;

 private:
  class IoSide;
  class Handle;

  void OnWatchMessage(int id, std::string change);
  void OnWatchEnd(int id, Status status);
  void StopWatch(int id);

  base::SequenceBound<IoSide> io_side_;
  int next_watch_id_ = 1;
  std::map<int, raw_ptr<WatchObserver>> watches_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CrossThreadTransport> weak_factory_{this};
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_CROSS_THREAD_TRANSPORT_H_

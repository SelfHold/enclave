// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/loopback_bridge.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"

namespace enclave::shard {

namespace {

void RunOnListener(base::WeakPtr<LoopbackListener> listener,
                   base::OnceCallback<void(LoopbackListener&)> task) {
  if (listener) {
    std::move(task).Run(*listener);
  }
}

void RunOnReplica(base::WeakPtr<LoopbackReplica> replica,
                  base::OnceCallback<void(LoopbackReplica&)> task) {
  if (replica) {
    std::move(task).Run(*replica);
  }
}

}  // namespace

LoopbackBridge::Slot::Slot() = default;
LoopbackBridge::Slot::Slot(const Slot&) = default;
LoopbackBridge::Slot::~Slot() = default;

// static
LoopbackBridge& LoopbackBridge::Get() {
  static base::NoDestructor<LoopbackBridge> bridge;
  return *bridge;
}

LoopbackBridge::LoopbackBridge() = default;
LoopbackBridge::~LoopbackBridge() = default;

void LoopbackBridge::RegisterReplica(const base::FilePath& file,
                                     base::WeakPtr<LoopbackReplica> replica) {
  base::AutoLock lock(lock_);
  Slot& slot = slots_[file];
  slot.replica = std::move(replica);
  slot.replica_runner = base::SequencedTaskRunner::GetCurrentDefault();
  PostToListener(slot, base::BindOnce([](LoopbackListener& listener) {
                   listener.OnReplicaAvailable();
                 }));
}

void LoopbackBridge::UnregisterReplica(const base::FilePath& file) {
  base::AutoLock lock(lock_);
  auto it = slots_.find(file);
  if (it == slots_.end()) {
    return;
  }
  it->second.replica = nullptr;
  it->second.replica_runner = nullptr;
  PostToListener(it->second, base::BindOnce([](LoopbackListener& listener) {
                   listener.OnReplicaGone();
                 }));
  if (!it->second.listener_runner) {
    slots_.erase(it);
  }
}

void LoopbackBridge::NotifyLocalCommit(const base::FilePath& file,
                                       LoopbackReplica::Entities entities) {
  base::AutoLock lock(lock_);
  auto it = slots_.find(file);
  if (it == slots_.end() || entities.empty()) {
    return;
  }
  PostToListener(it->second,
                 base::BindOnce(
                     [](LoopbackReplica::Entities entities,
                        LoopbackListener& listener) {
                       listener.OnLocalCommit(std::move(entities));
                     },
                     std::move(entities)));
}

void LoopbackBridge::SetListener(const base::FilePath& file,
                                 base::WeakPtr<LoopbackListener> listener) {
  base::AutoLock lock(lock_);
  Slot& slot = slots_[file];
  slot.listener = std::move(listener);
  slot.listener_runner = base::SequencedTaskRunner::GetCurrentDefault();
  if (slot.replica_runner) {
    PostToListener(slot, base::BindOnce([](LoopbackListener& listener) {
                     listener.OnReplicaAvailable();
                   }));
  }
}

void LoopbackBridge::ClearListener(const base::FilePath& file) {
  base::AutoLock lock(lock_);
  auto it = slots_.find(file);
  if (it == slots_.end()) {
    return;
  }
  it->second.listener = nullptr;
  it->second.listener_runner = nullptr;
  if (!it->second.replica_runner) {
    slots_.erase(it);
  }
}

bool LoopbackBridge::PostToReplica(
    const base::FilePath& file,
    base::OnceCallback<void(LoopbackReplica&)> task) {
  base::AutoLock lock(lock_);
  auto it = slots_.find(file);
  if (it == slots_.end() || !it->second.replica_runner) {
    return false;
  }
  return it->second.replica_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&RunOnReplica, it->second.replica, std::move(task)));
}

void LoopbackBridge::PostToListener(
    const Slot& slot,
    base::OnceCallback<void(LoopbackListener&)> task) {
  if (!slot.listener_runner) {
    return;
  }
  slot.listener_runner->PostTask(
      FROM_HERE,
      base::BindOnce(&RunOnListener, slot.listener, std::move(task)));
}

}  // namespace enclave::shard

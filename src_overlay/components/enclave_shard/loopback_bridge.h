// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_LOOPBACK_BRIDGE_H_
#define COMPONENTS_ENCLAVE_SHARD_LOOPBACK_BRIDGE_H_

#include <cstdint>
#include <map>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "components/sync/protocol/loopback_server.pb.h"

namespace enclave::shard {

// The sync engine's LoopbackServer, seen from the replicator. Lives on the
// sync engine's sequence; reached only through LoopbackBridge::PostToReplica.
class LoopbackReplica {
 public:
  using Entities = std::vector<sync_pb::LoopbackServerEntity>;

  virtual ~LoopbackReplica() = default;

  // Every entity the server holds, permanent ones included.
  virtual Entities ExportEntities() = 0;
  // Stores each entity as it is, with a new server version so that the
  // client gets it at its next GetUpdates. An entity replaces the one with
  // the same id. Does not count as a commit (no OnLocalCommit). Returns the
  // specifics field numbers of the types that changed.
  virtual std::vector<int> ImportEntities(Entities entities) = 0;
  // Throws the server state away and starts again with this keystore key and
  // a new store birthday; the client then merges its local data in again.
  virtual void ResetWithKeystoreKey(std::vector<uint8_t> keystore_key) = 0;
  virtual std::vector<std::vector<uint8_t>> GetKeystoreKeys() = 0;
};

// Told about the replica of one profile. Lives on the sequence that called
// LoopbackBridge::SetListener.
class LoopbackListener {
 public:
  virtual ~LoopbackListener() = default;

  // The sync engine started (or restarted) its server.
  virtual void OnReplicaAvailable() = 0;
  // The client committed these entities (as stored by the server).
  virtual void OnLocalCommit(LoopbackReplica::Entities entities) = 0;
  // The engine stopped; the replica is gone until OnReplicaAvailable.
  virtual void OnReplicaGone() = 0;
};

// Connects the LoopbackServer of a profile (created deep inside the sync
// engine, on its own sequence) with the profile's Shard replicator (on
// the UI thread). Keyed by the server's persistent file, which both sides
// know. Thread-safe.
class LoopbackBridge {
 public:
  static LoopbackBridge& Get();

  LoopbackBridge(const LoopbackBridge&) = delete;
  LoopbackBridge& operator=(const LoopbackBridge&) = delete;

  // --- replica side (sync engine sequence) ---------------------------------
  void RegisterReplica(const base::FilePath& file,
                       base::WeakPtr<LoopbackReplica> replica);
  void UnregisterReplica(const base::FilePath& file);
  void NotifyLocalCommit(const base::FilePath& file,
                         LoopbackReplica::Entities entities);

  // --- listener side --------------------------------------------------------
  // Replaces any earlier listener. If a replica is already registered, the
  // listener gets OnReplicaAvailable right away (posted).
  void SetListener(const base::FilePath& file,
                   base::WeakPtr<LoopbackListener> listener);
  void ClearListener(const base::FilePath& file);
  // Runs `task` on the replica's sequence. False when there is no replica
  // (the task is dropped). Replies are the task's business (BindPostTask).
  bool PostToReplica(const base::FilePath& file,
                     base::OnceCallback<void(LoopbackReplica&)> task);

 private:
  friend class base::NoDestructor<LoopbackBridge>;

  struct Slot {
    Slot();
    Slot(const Slot&);
    ~Slot();

    base::WeakPtr<LoopbackReplica> replica;
    scoped_refptr<base::SequencedTaskRunner> replica_runner;
    base::WeakPtr<LoopbackListener> listener;
    scoped_refptr<base::SequencedTaskRunner> listener_runner;
  };

  LoopbackBridge();
  ~LoopbackBridge();

  void PostToListener(const Slot& slot,
                      base::OnceCallback<void(LoopbackListener&)> task)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);

  base::Lock lock_;
  std::map<base::FilePath, Slot> slots_ GUARDED_BY(lock_);
};

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_LOOPBACK_BRIDGE_H_

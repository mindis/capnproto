// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "rpc.h"
#include "capability-context.h"
#include <kj/debug.h>
#include <kj/vector.h>
#include <kj/async.h>
#include <kj/one-of.h>
#include <kj/function.h>
#include <unordered_map>
#include <map>
#include <queue>
#include <capnp/rpc.capnp.h>

namespace capnp {
namespace _ {  // private

namespace {

template <typename T>
inline constexpr uint messageSizeHint() {
  return 1 + sizeInWords<rpc::Message>() + sizeInWords<T>();
}
template <>
inline constexpr uint messageSizeHint<void>() {
  return 1 + sizeInWords<rpc::Message>();
}

kj::Maybe<kj::Array<PipelineOp>> toPipelineOps(List<rpc::PromisedAnswer::Op>::Reader ops) {
  auto result = kj::heapArrayBuilder<PipelineOp>(ops.size());
  for (auto opReader: ops) {
    PipelineOp op;
    switch (opReader.which()) {
      case rpc::PromisedAnswer::Op::NOOP:
        op.type = PipelineOp::NOOP;
        break;
      case rpc::PromisedAnswer::Op::GET_POINTER_FIELD:
        op.type = PipelineOp::GET_POINTER_FIELD;
        op.pointerIndex = opReader.getGetPointerField();
        break;
      default:
        // TODO(soon):  Handle better?
        KJ_FAIL_REQUIRE("Unsupported pipeline op.", (uint)opReader.which()) {
          return nullptr;
        }
    }
  }
  return result.finish();
}

Orphan<List<rpc::PromisedAnswer::Op>> fromPipelineOps(
    Orphanage orphanage, kj::ArrayPtr<const PipelineOp> ops) {
  auto result = orphanage.newOrphan<List<rpc::PromisedAnswer::Op>>(ops.size());
  auto builder = result.get();
  for (uint i: kj::indices(ops)) {
    rpc::PromisedAnswer::Op::Builder opBuilder = builder[i];
    switch (ops[i].type) {
      case PipelineOp::NOOP:
        opBuilder.setNoop();
        break;
      case PipelineOp::GET_POINTER_FIELD:
        opBuilder.setGetPointerField(ops[i].pointerIndex);
        break;
    }
  }
  return result;
}

kj::Exception toException(const rpc::Exception::Reader& exception) {
  kj::Exception::Nature nature =
      exception.getIsCallersFault()
          ? kj::Exception::Nature::PRECONDITION
          : kj::Exception::Nature::LOCAL_BUG;

  kj::Exception::Durability durability;
  switch (exception.getDurability()) {
    default:
    case rpc::Exception::Durability::PERMANENT:
      durability = kj::Exception::Durability::PERMANENT;
      break;
    case rpc::Exception::Durability::TEMPORARY:
      durability = kj::Exception::Durability::TEMPORARY;
      break;
    case rpc::Exception::Durability::OVERLOADED:
      durability = kj::Exception::Durability::OVERLOADED;
      break;
  }

  return kj::Exception(nature, durability, "(remote)", 0, kj::heapString(exception.getReason()));
}

void fromException(const kj::Exception& exception, rpc::Exception::Builder builder) {
  builder.setReason(exception.getDescription());
  builder.setIsCallersFault(exception.getNature() == kj::Exception::Nature::PRECONDITION);
  switch (exception.getDurability()) {
    case kj::Exception::Durability::PERMANENT:
      builder.setDurability(rpc::Exception::Durability::PERMANENT);
      break;
    case kj::Exception::Durability::TEMPORARY:
      builder.setDurability(rpc::Exception::Durability::TEMPORARY);
      break;
    case kj::Exception::Durability::OVERLOADED:
      builder.setDurability(rpc::Exception::Durability::OVERLOADED);
      break;
  }
}

// =======================================================================================

typedef uint32_t QuestionId;
typedef uint32_t ExportId;

template <typename Id, typename T>
class ExportTable {
  // Table mapping integers to T, where the integers are chosen locally.

public:
  kj::Maybe<T&> find(Id id) {
    if (id < slots.size() && slots[id] != nullptr) {
      return slots[id];
    } else {
      return nullptr;
    }
  }

  bool erase(Id id) {
    if (id < slots.size() && slots[id] != nullptr) {
      slots[id] = T();
      freeIds.push(id);
      return true;
    } else {
      return false;
    }
  }

  T& next(Id& id) {
    if (freeIds.empty()) {
      id = slots.size();
      return slots.add();
    } else {
      id = freeIds.top();
      freeIds.pop();
      return slots[id];
    }
  }

private:
  kj::Vector<T> slots;
  std::priority_queue<Id, std::vector<Id>, std::greater<Id>> freeIds;
};

template <typename Id, typename T>
class ImportTable {
  // Table mapping integers to T, where the integers are chosen remotely.

public:
  T& operator[](Id id) {
    if (id < kj::size(low)) {
      return low[id];
    } else {
      return high[id];
    }
  }

  kj::Maybe<T&> find(Id id) {
    if (id < kj::size(low)) {
      return low[id];
    } else {
      auto iter = high.find(id);
      if (iter == high.end()) {
        return nullptr;
      } else {
        return iter->second;
      }
    }
  }

  void erase(Id id) {
    if (id < kj::size(low)) {
      low[id] = T();
    } else {
      high.erase(id);
    }
  }

private:
  T low[16];
  std::unordered_map<Id, T> high;
};

template <typename ParamCaps, typename RpcPipeline, typename RpcResponse>
struct Question {
  kj::Own<kj::PromiseFulfiller<kj::Own<RpcResponse>>> fulfiller;
  // Fulfill with the response.

  kj::Maybe<kj::Own<ParamCaps>> paramCaps;
  // CapInjector from the parameter struct.  This will be released once the `Return` message is
  // received and `retainedCaps` processed.  (If this is non-null, then the call has not returned
  // yet.)

  kj::Maybe<RpcPipeline&> pipeline;
  // The local pipeline object.  The RpcPipeline's own destructor sets this value to null.
  //
  // TODO(cleanup):  We only have this pointer here because CapInjectorImpl::getInjectedCap() needs
  //   it, but perhaps CapInjectorImpl should instead hold on to the ClientHook it got in the first
  //   place.

  bool isStarted = false;
  // Is this Question ID currently in-use?  (This is true until both `Return` has been received and
  // `Finish` has been sent.)

  bool isFinished = false;
  // Has the `Finish` message been sent?

  inline bool operator==(decltype(nullptr)) const { return !isStarted; }
  inline bool operator!=(decltype(nullptr)) const { return isStarted; }
};

template <typename CallContext>
struct Answer {
  bool active = false;
  // True from the point when the Call message is received to the point when both the `Finish`
  // message has been received and the `Return` has been sent.

  kj::Maybe<kj::Own<const PipelineHook>> pipeline;
  // Send pipelined calls here.  Becomes null as soon as a `Finish` is received.

  kj::Promise<void> asyncOp = nullptr;
  // Delete this promise to cancel the call.

  kj::Maybe<const CallContext&> callContext;
  // The call context, if it's still active.  Becomes null when the `Return` message is sent.  This
  // object, if non-null, is owned by `asyncOp`.
};

struct Export {
  uint refcount = 0;
  // When this reaches 0, drop `clientHook` and free this export.

  kj::Own<ClientHook> clientHook;

  inline bool operator==(decltype(nullptr)) const { return refcount == 0; }
  inline bool operator!=(decltype(nullptr)) const { return refcount != 0; }
};

template <typename ImportClient>
struct Import {
  ImportClient* client = nullptr;
  // Normally I'd want this to be Maybe<ImportClient&>, but GCC's unordered_map doesn't seem to
  // like DisableConstCopy types.
};

// =======================================================================================

class RpcConnectionState: public kj::TaskSet::ErrorHandler {
public:
  RpcConnectionState(const kj::EventLoop& eventLoop,
                     kj::Own<VatNetworkBase::Connection>&& connection)
      : eventLoop(eventLoop), connection(kj::mv(connection)), tasks(eventLoop, *this),
        exportDisposer(*this) {
    tasks.add(messageLoop());
  }

  void taskFailed(kj::Exception&& exception) override {
    // TODO(now):  Kill the connection.
  }

private:
  const kj::EventLoop& eventLoop;
  kj::Own<VatNetworkBase::Connection> connection;

  class ImportClient;
  class CapInjectorImpl;
  class CapExtractorImpl;
  class RpcPipeline;
  class RpcCallContext;
  class RpcResponse;

  struct Tables {
    ExportTable<QuestionId, Question<CapInjectorImpl, RpcPipeline, RpcResponse>> questions;
    ImportTable<QuestionId, Answer<RpcCallContext>> answers;
    ExportTable<ExportId, Export> exports;
    ImportTable<ExportId, Import<ImportClient>> imports;
  };
  kj::MutexGuarded<Tables> tables;

  kj::TaskSet tasks;


  class ExportDisposer final: public kj::Disposer {
  public:
    inline ExportDisposer(const RpcConnectionState& connectionState)
        : connectionState(connectionState) {}

  protected:
    void disposeImpl(void* pointer) const override {
      auto lock = connectionState.tables.lockExclusive();
      ExportId id = reinterpret_cast<intptr_t>(pointer);
      KJ_IF_MAYBE(exp, lock->exports.find(id)) {
        if (--exp->refcount == 0) {
          KJ_ASSERT(lock->exports.erase(id)) {
            break;
          }
        }
      } else {
        KJ_FAIL_REQUIRE("invalid export ID", id) { break; }
      }
    }

  private:
    const RpcConnectionState& connectionState;
  };

  ExportDisposer exportDisposer;

  // =====================================================================================
  // ClientHook implementations

  class RpcClient: public ClientHook, public kj::Refcounted {
  public:
    RpcClient(const RpcConnectionState& connectionState)
        : connectionState(connectionState) {}

    virtual void writeDescriptor(rpc::CapDescriptor::Builder descriptor, Tables& tables) const = 0;
    // Writes a CapDescriptor referencing this client.  Must be called with the
    // RpcConnectionState's table locked -- a reference to them is passed as the second argument.
    // The CapDescriptor must be sent before unlocking the tables, as it may become invalid at
    // any time once the tables are unlocked.

    virtual kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const = 0;
    // Writes the appropriate call target for calls to this capability and returns null.
    //
    // - OR -
    //
    // If calls have been redirected to some other local ClientHook, returns that hook instead.
    // This can happen if the capability represents a promise that has been resolved.

    // implements ClientHook -----------------------------------------

    VoidPromiseAndPipeline call(uint64_t interfaceId, uint16_t methodId,
                                kj::Own<CallContextHook>&& context) const override {
      auto params = context->getParams();

      size_t sizeHint = params.targetSizeInWords();

      // TODO(perf):  Extend targetSizeInWords() to include a capability count?  Here we increase
      //   the size by 1/16 to deal with cap descriptors possibly expanding.  See also below, when
      //   handling the response, and in RpcRequest::send().
      sizeHint += sizeHint / 16;

      // Don't overflow.
      if (uint(sizeHint) != sizeHint) {
        sizeHint = ~uint(0);
      }

      auto request = newCall(interfaceId, methodId, sizeHint);

      request.set(context->getParams());
      context->releaseParams();

      auto promise = request.send();

      auto pipeline = promise.releasePipelineHook();

      auto voidPromise = promise.then(kj::mvCapture(context,
          [](kj::Own<CallContextHook>&& context, Response<ObjectPointer> response) {
            size_t sizeHint = response.targetSizeInWords();

            // See above TODO.
            sizeHint += sizeHint / 16;

            // Don't overflow.
            if (uint(sizeHint) != sizeHint) {
              sizeHint = ~uint(0);
            }

            context->getResults(sizeHint).set(response);
          }));

      return { kj::mv(voidPromise), kj::mv(pipeline) };
    }

    kj::Own<const ClientHook> addRef() const override {
      return kj::addRef(*this);
    }
    const void* getBrand() const override {
      return &connectionState;
    }

  protected:
    const RpcConnectionState& connectionState;
  };

  class ImportClient: public RpcClient {
  protected:
    ImportClient(const RpcConnectionState& connectionState, ExportId importId)
        : RpcClient(connectionState), importId(importId) {}

  public:
    ~ImportClient() noexcept(false) {
      {
        // Remove self from the import table, if the table is still pointing at us.  (It's possible
        // that another thread attempted to obtain this import just as the destructor started, in
        // which case that other thread will have constructed a new ImportClient and placed it in
        // the import table.)
        auto lock = connectionState.tables.lockExclusive();
        KJ_IF_MAYBE(import, lock->imports.find(importId)) {
          if (import->client == this) {
            lock->imports.erase(importId);
          }
        }
      }

      // Send a message releasing our remote references.
      if (remoteRefcount > 0) {
        connectionState.sendReleaseLater(importId, remoteRefcount);
      }
    }

    virtual bool settle(kj::Own<const ClientHook> replacement) = 0;
    // Replace the PromiseImportClient with its resolution.  Returns false if this is not a promise
    // (i.e. it is a SettledImportClient).

    kj::Maybe<kj::Own<ImportClient>> tryAddRemoteRef() {
      // Add a new RemoteRef and return a new ref to this client representing it.  Returns null
      // if this client is being deleted in another thread, in which case the caller should
      // construct a new one.

      KJ_IF_MAYBE(ref, kj::tryAddRef(*this)) {
        ++remoteRefcount;
        return kj::mv(*ref);
      } else {
        return nullptr;
      }
    }

    void writeDescriptor(rpc::CapDescriptor::Builder descriptor, Tables& tables) const override {
      descriptor.setReceiverHosted(importId);
    }

    kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const override {
      target.setExportedCap(importId);
      return nullptr;
    }

    // implements ClientHook -----------------------------------------
    Request<ObjectPointer, ObjectPointer> newCall(
        uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) const override {
      auto request = kj::heap<RpcRequest>(connectionState, firstSegmentWordSize, kj::addRef(*this));
      auto callBuilder = request->getCall();

      callBuilder.getTarget().setExportedCap(importId);
      callBuilder.setInterfaceId(interfaceId);
      callBuilder.setMethodId(methodId);

      auto root = request->getRoot();
      return Request<ObjectPointer, ObjectPointer>(root, kj::mv(request));
    }

  private:
    ExportId importId;

    uint remoteRefcount = 0;
    // Number of times we've received this import from the peer.
  };

  class SettledImportClient final: public ImportClient {
  public:
    inline SettledImportClient(const RpcConnectionState& connectionState, ExportId importId)
        : ImportClient(connectionState, importId) {}

    bool settle(kj::Own<const ClientHook> replacement) override {
      return false;
    }

    kj::Maybe<kj::Promise<kj::Own<const ClientHook>>> whenMoreResolved() const override {
      return nullptr;
    }
  };

  class PromiseImportClient final: public ImportClient {
  public:
    PromiseImportClient(const RpcConnectionState& connectionState, ExportId importId)
        : ImportClient(connectionState, importId),
          fork(nullptr) {
      auto paf = kj::newPromiseAndFulfiller<kj::Own<const ClientHook>>(connectionState.eventLoop);
      fulfiller = kj::mv(paf.fulfiller);
      fork = paf.promise.fork();
    }

    bool settle(kj::Own<const ClientHook> replacement) override {
      fulfiller->fulfill(kj::mv(replacement));
      return true;
    }

    // TODO(now):  Override writeDescriptor() and writeTarget() to redirect once the promise
    //   resolves.

    kj::Maybe<kj::Promise<kj::Own<const ClientHook>>> whenMoreResolved() const override {
      // We need the returned promise to hold a reference back to this object, so that it doesn't
      // disappear while the promise is still outstanding.
      return fork.addBranch().thenInAnyThread(kj::mvCapture(kj::addRef(*this),
          [](kj::Own<const PromiseImportClient>&&, kj::Own<const ClientHook>&& replacement) {
            return kj::mv(replacement);
          }));
    }

  private:
    kj::Own<kj::PromiseFulfiller<kj::Own<const ClientHook>>> fulfiller;
    kj::ForkedPromise<kj::Own<const ClientHook>> fork;
  };

  class PromisedAnswerClient final: public RpcClient {
  public:
    PromisedAnswerClient(const RpcConnectionState& connectionState,
                         kj::Own<const RpcPipeline>&& pipeline,
                         kj::Array<PipelineOp>&& ops)
        : RpcClient(connectionState), ops(kj::mv(ops)),
          resolveSelfPromise(pipeline->onResponse().then(
              [this](kj::Own<const RpcResponse>&& response) -> kj::Promise<void> {
                resolve(kj::mv(response));
                return kj::READY_NOW;  // hack to force eager resolution.
              }, [this](kj::Exception&& exception) -> kj::Promise<void> {
                resolve(kj::mv(exception));
                return kj::READY_NOW;  // hack to force eager resolution.
              })) {
      state.getWithoutLock().init<Waiting>(kj::mv(pipeline));
    }

    void writeDescriptor(rpc::CapDescriptor::Builder descriptor, Tables& tables) const override {
      auto lock = state.lockShared();

      if (lock->is<Waiting>()) {
        return lock->get<Waiting>()->writeDescriptor(descriptor, tables, ops);
      } else if (lock->is<Resolved>()) {
        return connectionState.writeDescriptor(lock->get<Resolved>()->addRef(), descriptor, tables);
      } else {
        // TODO(now)
      }
    }

    kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target) const override {
      auto lock = state.lockShared();

      if (lock->is<Waiting>()) {
        return lock->get<Waiting>()->writeTarget(target, ops);
      } else if (lock->is<Resolved>()) {
        return connectionState.writeTarget(*lock->get<Resolved>(), target);
      } else {
        return newBrokenCap(kj::cp(lock->get<Broken>()));
      }
    }

    // implements ClientHook -----------------------------------------

    Request<ObjectPointer, ObjectPointer> newCall(
        uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) const override {
      auto lock = state.lockShared();

      if (lock->is<Waiting>()) {
        auto request = kj::heap<RpcRequest>(
            connectionState, firstSegmentWordSize, kj::addRef(*this));
        auto callBuilder = request->getCall();

        callBuilder.setInterfaceId(interfaceId);
        callBuilder.setMethodId(methodId);

        auto root = request->getRoot();
        return Request<ObjectPointer, ObjectPointer>(root, kj::mv(request));
      } else if (lock->is<Resolved>()) {
        return lock->get<Resolved>()->newCall(interfaceId, methodId, firstSegmentWordSize);
      } else {
        return newBrokenCap(kj::cp(lock->get<Broken>()))->newCall(
            interfaceId, methodId, firstSegmentWordSize);
      }
    }

    kj::Maybe<kj::Promise<kj::Own<const ClientHook>>> whenMoreResolved() const override {
      auto lock = state.lockShared();

      if (lock->is<Waiting>()) {
        return lock->get<Waiting>()->onResponse().then(kj::mvCapture(kj::heapArray(ops.asPtr()),
            [](kj::Array<PipelineOp>&& ops, kj::Own<const RpcResponse>&& response) {
              return response->getResults().getPipelinedCap(ops);
            }));
      } else if (lock->is<Resolved>()) {
        return kj::Promise<kj::Own<const ClientHook>>(lock->get<Resolved>()->addRef());
      } else {
        return kj::Promise<kj::Own<const ClientHook>>(kj::cp(lock->get<Broken>()));
      }
    }

  private:
    kj::Array<PipelineOp> ops;

    typedef kj::Own<const RpcPipeline> Waiting;
    typedef kj::Own<const ClientHook> Resolved;
    typedef kj::Exception Broken;
    kj::MutexGuarded<kj::OneOf<Waiting, Resolved, Broken>> state;

    // Keep this last, because the continuation uses *this, so it should be destroyed first to
    // ensure the continuation is not still running.
    kj::Promise<void> resolveSelfPromise;

    void resolve(kj::Own<const RpcResponse>&& response) {
      auto lock = state.lockExclusive();
      KJ_ASSERT(lock->is<Waiting>(), "Already resolved?");
      lock->init<Resolved>(response->getResults().getPipelinedCap(ops));
    }

    void resolve(const kj::Exception&& exception) {
      auto lock = state.lockExclusive();
      KJ_ASSERT(lock->is<Waiting>(), "Already resolved?");
      lock->init<Broken>(kj::mv(exception));
    }
  };

  void writeDescriptor(kj::Own<const ClientHook> cap, rpc::CapDescriptor::Builder descriptor,
                       Tables& tables) const {
    // Write a descriptor for the given capability.  The tables must be locked by the caller and
    // passed in as a parameter.

    if (cap->getBrand() == this) {
      kj::downcast<const RpcClient>(*cap).writeDescriptor(descriptor, tables);
    } else {
      // TODO(now):  We have to figure out if the client is already in our table.
      // TODO(now):  We have to add a refcount to the export, and return an object that decrements
      //   that refcount later.
    }
  }

  kj::Maybe<kj::Own<const ClientHook>> writeTarget(
      const ClientHook& cap, rpc::Call::Target::Builder target) const {
    // If calls to the given capability should pass over this connection, fill in `target`
    // appropriately for such a call and return nullptr.  Otherwise, return a `ClientHook` to which
    // the call should be forwarded; the caller should then delegate the call to that `ClientHook`.
    //
    // The main case where this ends up returning non-null is if `cap` is a promise that has
    // recently resolved.  The application might have started building a request before the promise
    // resolved, and so the request may have been built on the assumption that it would be sent over
    // this network connection, but then the promise resolved to point somewhere else before the
    // request was sent.  Now the request has to be redirected to the new target instead.

    if (cap.getBrand() == this) {
      return kj::downcast<const RpcClient>(cap).writeTarget(target);
    } else {
      return cap.addRef();
    }
  }

  // =====================================================================================
  // CapExtractor / CapInjector implementations

  class CapExtractorImpl final: public CapExtractor<rpc::CapDescriptor> {
    // Reads CapDescriptors from a received message.

  public:
    CapExtractorImpl(const RpcConnectionState& connectionState)
        : connectionState(connectionState) {}

    ~CapExtractorImpl() noexcept(false) {
      KJ_ASSERT(retainedCaps.getWithoutLock().size() > 0,
                "CapExtractorImpl destroyed without getting a chance to retain the caps!") {
        break;
      }
    }

    uint retainedListSizeHint(bool final) {
      // Get the expected size of the retained caps list, in words.  If `final` is true, then it
      // is known that no more caps will be extracted after this point, so an exact value can be
      // returned.  Otherwise, the returned size includes room for error.

      // If `final` is true then there's no need to lock.  If it is false, then asynchronous
      // access is possible.  It's probably not worth taking the lock to look; we'll just return
      // a silly estimate.
      uint count = final ? retainedCaps.getWithoutLock().size() : 32;
      return (count * sizeof(ExportId) + (sizeof(ExportId) - 1)) / sizeof(word);
    }

    Orphan<List<ExportId>> finalizeRetainedCaps(Orphanage orphanage) {
      // Called on finalization, when the lock is no longer needed.
      kj::Vector<ExportId> retainedCaps = kj::mv(this->retainedCaps.getWithoutLock());

      auto lock = connectionState.tables.lockExclusive();

      auto actualRetained = retainedCaps.begin();
      for (ExportId importId: retainedCaps) {
        // Check if the import still exists under this ID.
        KJ_IF_MAYBE(import, lock->imports.find(importId)) {
          if (import->client != nullptr && import->client->tryAddRemoteRef() != nullptr) {
            // Import indeed still exists!  We are responsible for retaining it.
            *actualRetained++ = importId;
          }
        }
      }

      uint count = actualRetained - retainedCaps.begin();

      // Build the retain list out of the imports that had non-zero refcounts.
      auto result = orphanage.newOrphan<List<ExportId>>(count);
      auto resultBuilder = result.get();
      count = 0;
      for (auto iter = retainedCaps.begin(); iter < actualRetained; ++iter) {
        resultBuilder.set(count++, *iter);
      }

      return kj::mv(result);
    }

    // implements CapDescriptor ------------------------------------------------

    kj::Own<const ClientHook> extractCap(rpc::CapDescriptor::Reader descriptor) const override {
      switch (descriptor.which()) {
        case rpc::CapDescriptor::SENDER_HOSTED:
        case rpc::CapDescriptor::SENDER_PROMISE: {
          ExportId importId = descriptor.getSenderHosted();

          auto lock = connectionState.tables.lockExclusive();

          auto& import = lock->imports[importId];
          if (import.client != nullptr) {
            // The import is already on the table, but it could be being deleted in another
            // thread.
            KJ_IF_MAYBE(ref, kj::tryAddRef(*import.client)) {
              // We successfully grabbed a reference to the import without it being deleted in
              // another thread.  Since this import already exists, we don't have to take
              // responsibility for retaining it.  We can just return the existing object and
              // be done with it.
              return kj::mv(*ref);
            }
          }

          // No import for this ID exists currently, so create one.
          kj::Own<ImportClient> result;
          if (descriptor.which() == rpc::CapDescriptor::SENDER_PROMISE) {
            // TODO(now):  Check for pending `Resolve` messages replacing this import ID, and if
            //   one exists, use that client instead.
            kj::refcounted<PromiseImportClient>(connectionState, importId);
          } else {
            kj::refcounted<SettledImportClient>(connectionState, importId);
          }
          import.client = result;

          // Note that we need to retain this import later if it still exists.
          retainedCaps.lockExclusive()->add(importId);

          return kj::mv(result);
        }

        case rpc::CapDescriptor::RECEIVER_HOSTED: {
          auto lock = connectionState.tables.lockExclusive();  // TODO(perf): shared?
          KJ_IF_MAYBE(exp, lock->exports.find(descriptor.getReceiverHosted())) {
            return exp->clientHook->addRef();
          }
          return newBrokenCap("invalid 'receiverHosted' export ID");
        }

        case rpc::CapDescriptor::RECEIVER_ANSWER: {
          // TODO(now): implement
          return newBrokenCap("'receiverAnswer' not implemented");
        }

        case rpc::CapDescriptor::THIRD_PARTY_HOSTED:
          return newBrokenCap("three-way introductions not implemented");

        default:
          return newBrokenCap("unknown CapDescriptor type");
      }
    }

  private:
    const RpcConnectionState& connectionState;

    kj::MutexGuarded<kj::Vector<ExportId>> retainedCaps;
    // Imports which we are responsible for retaining, should they still exist at the time that
    // this message is released.
  };

  // -----------------------------------------------------------------

  class CapInjectorImpl final: public CapInjector<rpc::CapDescriptor> {
    // Write CapDescriptors into a message as it is being built, before sending it.

  public:
    CapInjectorImpl(const RpcConnectionState& connectionState)
        : connectionState(connectionState) {}
    ~CapInjectorImpl() {}

    void finish(Tables& tables) {
      // Finish writing all of the CapDescriptors.  Must be called with the tables locked, and the
      // message must be sent before the tables are unlocked.

      for (auto& entry: caps.getWithoutLock()) {
        connectionState.writeDescriptor(kj::mv(entry.second.cap), entry.second.builder, tables);
      }
    }

    // implements CapInjector ----------------------------------------

    void injectCap(rpc::CapDescriptor::Builder descriptor,
                   kj::Own<const ClientHook>&& cap) const override {
      auto lock = caps.lockExclusive();
      auto result = lock->insert(std::make_pair(
          identity(descriptor), CapInfo(descriptor, kj::mv(cap))));
      KJ_REQUIRE(result.second, "A cap has already been injected at this location.") {
        result.first->second.cap = kj::mv(cap);
        break;
      }
    }
    kj::Own<const ClientHook> getInjectedCap(rpc::CapDescriptor::Reader descriptor) const override {
      auto lock = caps.lockExclusive();
      auto iter = lock->find(identity(descriptor));
      KJ_REQUIRE(iter != lock->end(), "getInjectedCap() called on descriptor I didn't write.");
      return iter->second.cap->addRef();

      switch (descriptor.which()) {
        case rpc::CapDescriptor::SENDER_HOSTED: {
          auto lock = connectionState.tables.lockExclusive();
          KJ_IF_MAYBE(exp, lock->exports.find(descriptor.getSenderHosted())) {
            return exp->clientHook->addRef();
          } else {
            KJ_FAIL_REQUIRE("Dropped descriptor had invalid 'senderHosted'.") {
              return newBrokenCap("Calling invalid CapDescriptor found in builder.");
            }
          }
        }

        case rpc::CapDescriptor::RECEIVER_HOSTED: {
          auto lock = connectionState.tables.lockExclusive();

          KJ_IF_MAYBE(import, lock->imports.find(descriptor.getReceiverHosted())) {
            if (import->client != nullptr) {
              KJ_IF_MAYBE(ref, kj::tryAddRef(*import->client)) {
                return kj::mv(*ref);
              }
            }
          }

          // If we wrote this CapDescriptor then we should hold a reference to the import in
          // our `receiverHosted` table, yet it seems that the import ID is not valid.  Something
          // is wrong.
          return newBrokenCap("CapDescriptor in builder had invalid 'receiverHosted'.");
        }

        case rpc::CapDescriptor::RECEIVER_ANSWER: {
          auto promisedAnswer = descriptor.getReceiverAnswer();
          auto lock = connectionState.tables.lockExclusive();

          KJ_IF_MAYBE(question, lock->questions.find(promisedAnswer.getQuestionId())) {
            KJ_IF_MAYBE(pipeline, question->pipeline) {
              KJ_IF_MAYBE(ops, toPipelineOps(promisedAnswer.getTransform())) {
                return pipeline->getPipelinedCap(kj::mv(*ops));
              }
            }
          }
          return newBrokenCap("CapDescriptor in builder had invalid PromisedAnswer.");
        }

        default:
          KJ_FAIL_REQUIRE("I don't think I wrote this descriptor.") {
            return newBrokenCap("CapDescriptor in builder was invalid.");
          }
          break;
      }
    }
    void dropCap(rpc::CapDescriptor::Reader descriptor) const override {
      caps.lockExclusive()->erase(identity(descriptor));
    }

  private:
    const RpcConnectionState& connectionState;

    struct CapInfo {
      rpc::CapDescriptor::Builder builder;
      kj::Own<const ClientHook> cap;

      CapInfo(rpc::CapDescriptor::Builder& builder, kj::Own<const ClientHook>&& cap)
          : builder(builder), cap(kj::mv(cap)) {}

      CapInfo(const CapInfo& other);
      // Work around problem where std::pair complains about the copy constructor requiring a
      // non-const argument due to `builder` inheriting kj::DisableConstCopy.  The copy constructor
      // should never be called anyway.
    };

    kj::MutexGuarded<std::map<const void*, CapInfo>> caps;
    // Maps CapDescriptor locations to embedded caps.  The descriptors aren't actually filled in
    // until just before the message is sent.

    static const void* identity(const rpc::CapDescriptor::Reader& desc) {
      // TODO(cleanup):  Don't rely on internal APIs here.
      return _::PointerHelpers<rpc::CapDescriptor>::getInternalReader(desc).getLocation();
    }
  };

  // =====================================================================================
  // RequestHook/PipelineHook/ResponseHook implementations

  class QuestionRef: public kj::Refcounted {
  public:
    inline QuestionRef(const RpcConnectionState& connectionState, QuestionId id)
        : connectionState(connectionState), id(id), resultCaps(connectionState) {}

    ~QuestionRef() {
      // Send the "Finish" message.
      auto message = connectionState.connection->newOutgoingMessage(
          messageSizeHint<rpc::Finish>() + resultCaps.retainedListSizeHint(true));
      auto builder = message->getBody().getAs<rpc::Message>().initFinish();
      builder.setQuestionId(id);
      builder.adoptRetainedCaps(resultCaps.finalizeRetainedCaps(
          Orphanage::getForMessageContaining(builder)));

      message->send();

      // Check if the question has returned and, if so, remove it from the table.
      // Remove question ID from the table.  Must do this *after* sending `Finish` to ensure that
      // the ID is not re-allocated before the `Finish` message can be sent.
      {
        auto lock = connectionState.tables.lockExclusive();
        auto& question = KJ_ASSERT_NONNULL(
            lock->questions.find(id), "Question ID no longer on table?");
        if (question.paramCaps == nullptr) {
          // Call has already returned, so we can now remove it from the table.
          KJ_ASSERT(lock->questions.erase(id));
        } else {
          question.isFinished = true;
        }
      }
    }

    inline QuestionId getId() const { return id; }

    CapExtractorImpl& getResultCapExtractor() { return resultCaps; }

  private:
    const RpcConnectionState& connectionState;
    QuestionId id;
    CapExtractorImpl resultCaps;
  };

  class RpcRequest final: public RequestHook {
  public:
    RpcRequest(const RpcConnectionState& connectionState, uint firstSegmentWordSize,
               kj::Own<const RpcClient>&& target)
        : connectionState(connectionState),
          target(kj::mv(target)),
          message(connectionState.connection->newOutgoingMessage(
              firstSegmentWordSize == 0 ? 0 : firstSegmentWordSize + messageSizeHint<rpc::Call>())),
          injector(kj::heap<CapInjectorImpl>(connectionState)),
          context(*injector),
          callBuilder(message->getBody().getAs<rpc::Message>().initCall()),
          paramsBuilder(context.imbue(callBuilder.getRequest())) {}

    inline ObjectPointer::Builder getRoot() {
      return paramsBuilder;
    }
    inline rpc::Call::Builder getCall() {
      return callBuilder;
    }

    RemotePromise<ObjectPointer> send() override {
      QuestionId questionId;
      kj::Promise<kj::Own<RpcResponse>> promise = nullptr;

      {
        auto lock = connectionState.tables.lockExclusive();

        KJ_IF_MAYBE(redirect, target->writeTarget(callBuilder.getTarget())) {
          // Whoops, this capability has been redirected while we were building the request!
          // We'll have to make a new request and do a copy.  Ick.

          lock.release();

          size_t sizeHint = paramsBuilder.targetSizeInWords();

          // TODO(perf):  See TODO in RpcClient::call() about why we need to inflate the size a bit.
          sizeHint += sizeHint / 16;

          // Don't overflow.
          if (uint(sizeHint) != sizeHint) {
            sizeHint = ~uint(0);
          }

          auto replacement = redirect->get()->newCall(
              callBuilder.getInterfaceId(), callBuilder.getMethodId(), sizeHint);
          replacement.set(paramsBuilder);
          return replacement.send();
        } else {
          injector->finish(*lock);

          auto paf = kj::newPromiseAndFulfiller<kj::Own<RpcResponse>>(connectionState.eventLoop);
          auto& question = lock->questions.next(questionId);

          callBuilder.setQuestionId(questionId);
          question.isStarted = true;
          question.paramCaps = kj::mv(injector);
          question.fulfiller = kj::mv(paf.fulfiller);

          message->send();

          promise = kj::mv(paf.promise);
        }
      }

      auto questionRef = kj::refcounted<QuestionRef>(connectionState, questionId);

      auto promiseWithQuestionRef = promise.then(kj::mvCapture(kj::addRef(*questionRef),
          [](kj::Own<const QuestionRef>&& questionRef, kj::Own<RpcResponse>&& response)
              -> kj::Own<const RpcResponse> {
            response->setQuestionRef(kj::mv(questionRef));
            return kj::mv(response);
          }));

      auto forkedPromise = promiseWithQuestionRef.fork();

      auto appPromise = forkedPromise.addBranch().then([](kj::Own<const RpcResponse>&& response) {
        auto reader = response->getResults();
        return Response<ObjectPointer>(reader, kj::mv(response));
      });

      auto pipeline = kj::refcounted<RpcPipeline>(
          connectionState, questionId, kj::mv(forkedPromise));

      return RemotePromise<ObjectPointer>(
          kj::mv(appPromise),
          ObjectPointer::Pipeline(kj::mv(pipeline)));
    }

  private:
    const RpcConnectionState& connectionState;

    kj::Own<const RpcClient> target;
    kj::Own<OutgoingRpcMessage> message;
    kj::Own<CapInjectorImpl> injector;
    CapBuilderContext context;
    rpc::Call::Builder callBuilder;
    ObjectPointer::Builder paramsBuilder;
  };

  class RpcPipeline final: public PipelineHook, public kj::Refcounted {
  public:
    RpcPipeline(const RpcConnectionState& connectionState, QuestionId questionId,
                kj::ForkedPromise<kj::Own<const RpcResponse>>&& redirectLaterParam)
        : connectionState(connectionState),
          redirectLater(kj::mv(redirectLaterParam)),
          resolveSelfPromise(redirectLater.addBranch().then(
              [this](kj::Own<const RpcResponse>&& response) -> kj::Promise<void> {
                resolve(kj::mv(response));
                return kj::READY_NOW;  // hack to force eager resolution.
              }, [this](kj::Exception&& exception) -> kj::Promise<void> {
                resolve(kj::mv(exception));
                return kj::READY_NOW;
              })) {
      // Construct a new RpcPipeline.

      state.getWithoutLock().init<Waiting>(questionId);
    }

    kj::Promise<kj::Own<const RpcResponse>> onResponse() const {
      return redirectLater.addBranch();
    }

    kj::Maybe<kj::Own<const ClientHook>> writeTarget(
        rpc::Call::Target::Builder target, kj::ArrayPtr<const PipelineOp> ops) const {
      // Initializes `target` to a PromisedAnswer for a pipelined capability, *or* returns the
      // specified capability directly.
      //
      // The caller *should* have a lock on the connection state's tables while calling this, so
      // that a Finish message cannot be sent before the caller manages to send its `Call`.

      auto lock = state.lockExclusive();
      if (lock->is<Waiting>()) {
        auto builder = target.initPromisedAnswer();
        builder.setQuestionId(lock->get<Waiting>());
        builder.adoptTransform(fromPipelineOps(Orphanage::getForMessageContaining(builder), ops));
        return nullptr;
      } else if (lock->is<Resolved>()) {
        return lock->get<Resolved>()->getResults().getPipelinedCap(ops);
      } else {
        return newBrokenCap(kj::cp(lock->get<Broken>()));
      }
    }

    void writeDescriptor(rpc::CapDescriptor::Builder descriptor, Tables& tables,
                         kj::ArrayPtr<const PipelineOp> ops) const {
      auto lock = state.lockExclusive();
      if (lock->is<Waiting>()) {
        auto promisedAnswer = descriptor.initReceiverAnswer();
        promisedAnswer.setQuestionId(lock->get<Waiting>());
        promisedAnswer.adoptTransform(fromPipelineOps(
            Orphanage::getForMessageContaining(descriptor), ops));
      } else if (lock->is<Resolved>()) {
        connectionState.writeDescriptor(lock->get<Resolved>()->getResults().getPipelinedCap(ops),
                                        descriptor, tables);
      } else {
        connectionState.writeDescriptor(newBrokenCap(kj::cp(lock->get<Broken>())),
                                        descriptor, tables);
      }
    }

    // implements PipelineHook ---------------------------------------

    kj::Own<const PipelineHook> addRef() const override {
      return kj::addRef(*this);
    }

    kj::Own<const ClientHook> getPipelinedCap(kj::ArrayPtr<const PipelineOp> ops) const override {
      auto copy = kj::heapArrayBuilder<PipelineOp>(ops.size());
      for (auto& op: ops) {
        copy.add(op);
      }
      return getPipelinedCap(copy.finish());
    }

    kj::Own<const ClientHook> getPipelinedCap(kj::Array<PipelineOp>&& ops) const override {
      auto lock = state.lockExclusive();
      if (lock->is<Waiting>()) {
        return kj::refcounted<PromisedAnswerClient>(
            connectionState, kj::addRef(*this), kj::mv(ops));
      } else if (lock->is<Resolved>()) {
        return lock->get<Resolved>()->getResults().getPipelinedCap(ops);
      } else {
        return newBrokenCap(kj::cp(lock->get<Broken>()));
      }
    }

  private:
    const RpcConnectionState& connectionState;
    kj::Maybe<CapExtractorImpl&> capExtractor;
    kj::ForkedPromise<kj::Own<const RpcResponse>> redirectLater;

    typedef QuestionId Waiting;
    typedef kj::Own<const RpcResponse> Resolved;
    typedef kj::Exception Broken;
    kj::MutexGuarded<kj::OneOf<Waiting, Resolved, Broken>> state;

    // Keep this last, because the continuation uses *this, so it should be destroyed first to
    // ensure the continuation is not still running.
    kj::Promise<void> resolveSelfPromise;

    void resolve(kj::Own<const RpcResponse>&& response) {
      auto lock = state.lockExclusive();
      KJ_ASSERT(lock->is<Waiting>(), "Already resolved?");
      lock->init<Resolved>(kj::mv(response));
    }

    void resolve(const kj::Exception&& exception) {
      auto lock = state.lockExclusive();
      KJ_ASSERT(lock->is<Waiting>(), "Already resolved?");
      lock->init<Broken>(kj::mv(exception));
    }
  };

  class RpcResponse final: public ResponseHook, public kj::Refcounted {
  public:
    RpcResponse(const RpcConnectionState& connectionState,
                kj::Own<IncomingRpcMessage>&& message,
                ObjectPointer::Reader results)
        : message(kj::mv(message)),
          extractor(connectionState),
          context(extractor),
          reader(context.imbue(results)) {}

    ObjectPointer::Reader getResults() const {
      return reader;
    }

    kj::Own<const RpcResponse> addRef() const {
      return kj::addRef(*this);
    }

    void setQuestionRef(kj::Own<const QuestionRef>&& questionRef) {
      this->questionRef = kj::mv(questionRef);
    }

  private:
    kj::Own<IncomingRpcMessage> message;
    CapExtractorImpl extractor;
    CapReaderContext context;
    ObjectPointer::Reader reader;
    kj::Own<const QuestionRef> questionRef;
  };

  // =====================================================================================
  // CallContextHook implementation

  class RpcServerResponse {
  public:
    RpcServerResponse(const RpcConnectionState& connectionState,
                      kj::Own<OutgoingRpcMessage>&& message,
                      ObjectPointer::Builder results)
        : connectionState(connectionState),
          message(kj::mv(message)),
          injector(connectionState),
          context(injector),
          builder(context.imbue(results)) {}

    ObjectPointer::Builder getResults() {
      return builder;
    }

    void send() {
      auto lock = connectionState.tables.lockExclusive();
      injector.finish(*lock);
      message->send();
    }

  private:
    const RpcConnectionState& connectionState;
    kj::Own<OutgoingRpcMessage> message;
    CapInjectorImpl injector;
    CapBuilderContext context;
    ObjectPointer::Builder builder;
  };

  class RpcCallContext final: public CallContextHook, public kj::Refcounted {
  public:
    RpcCallContext(RpcConnectionState& connectionState, QuestionId questionId,
                   kj::Own<IncomingRpcMessage>&& request, const ObjectPointer::Reader& params)
        : connectionState(connectionState),
          questionId(questionId),
          request(kj::mv(request)),
          requestCapExtractor(connectionState),
          requestCapContext(requestCapExtractor),
          params(requestCapContext.imbue(params)),
          returnMessage(nullptr) {}

    void sendReturn() {
      if (isFirstResponder()) {
        if (response == nullptr) getResults(1);  // force initialization of response

        returnMessage.setQuestionId(questionId);
        returnMessage.adoptRetainedCaps(requestCapExtractor.finalizeRetainedCaps(
            Orphanage::getForMessageContaining(returnMessage)));

        KJ_ASSERT_NONNULL(response)->send();
      }
    }
    void sendErrorReturn(kj::Exception&& exception) {
      if (isFirstResponder()) {
        auto message = connectionState.connection->newOutgoingMessage(
            messageSizeHint<rpc::Return>() + sizeInWords<rpc::Exception>() +
            exception.getDescription().size() / sizeof(word) + 1);
        auto builder = message->getBody().initAs<rpc::Message>().initReturn();

        builder.setQuestionId(questionId);
        builder.adoptRetainedCaps(requestCapExtractor.finalizeRetainedCaps(
            Orphanage::getForMessageContaining(returnMessage)));
        fromException(exception, builder.initException());

        message->send();
      }
    }
    void sendCancel() {
      if (isFirstResponder()) {
        auto message = connectionState.connection->newOutgoingMessage(
            messageSizeHint<rpc::Return>());
        auto builder = message->getBody().initAs<rpc::Message>().initReturn();

        builder.setQuestionId(questionId);
        builder.adoptRetainedCaps(requestCapExtractor.finalizeRetainedCaps(
            Orphanage::getForMessageContaining(returnMessage)));
        builder.setCanceled();

        message->send();
      }
    }

    void requestCancel() const {
      // Hints that the caller wishes to cancel this call.  At the next time when cancellation is
      // deemed safe, the RpcCallContext shall send a canceled Return -- or if it never becomes
      // safe, the RpcCallContext will send a normal return when the call completes.  Either way
      // the RpcCallContext is now responsible for cleaning up the entry in the answer table, since
      // a Finish message was already received.

      // Verify that we're holding the tables mutex.  This is important because we're handing off
      // responsibility for deleting the answer.  Moreover, the callContext pointer in the answer
      // table should not be null as this would indicate that we've already returned a result.
      KJ_DASSERT(connectionState.tables.getAlreadyLockedExclusive()
                     .answers[questionId].callContext != nullptr);

      if (__atomic_fetch_or(&cancellationFlags, CANCEL_REQUESTED, __ATOMIC_RELAXED) ==
          CANCEL_ALLOWED) {
        // We just set CANCEL_REQUESTED, and CANCEL_ALLOWED was already set previously.  Schedule
        // the cancellation.
        scheduleCancel();
      }
    }

    // implements CallContextHook ------------------------------------

    ObjectPointer::Reader getParams() override {
      KJ_REQUIRE(request != nullptr, "Can't call getParams() after releaseParams().");
      return params;
    }
    void releaseParams() override {
      request = nullptr;
    }
    ObjectPointer::Builder getResults(uint firstSegmentWordSize) override {
      KJ_IF_MAYBE(r, response) {
        return r->get()->getResults();
      } else {
        auto message = connectionState.connection->newOutgoingMessage(
            firstSegmentWordSize == 0 ? 0 :
            firstSegmentWordSize + messageSizeHint<rpc::Return>() +
            requestCapExtractor.retainedListSizeHint(request == nullptr));
        returnMessage = message->getBody().initAs<rpc::Message>().initReturn();
        auto response = kj::heap<RpcServerResponse>(
            connectionState, kj::mv(message), returnMessage.getAnswer());
        auto results = response->getResults();
        this->response = kj::mv(response);
        return results;
      }
    }
    void allowAsyncCancellation() override {
      if (threadAcceptingCancellation != nullptr) {
        threadAcceptingCancellation = &kj::EventLoop::current();

        if (__atomic_fetch_or(&cancellationFlags, CANCEL_ALLOWED, __ATOMIC_RELAXED) ==
            CANCEL_REQUESTED) {
          // We just set CANCEL_ALLOWED, and CANCEL_REQUESTED was already set previously.  Schedule
          // the cancellation.
          scheduleCancel();
        }
      }
    }
    bool isCanceled() override {
      return __atomic_load_n(&cancellationFlags, __ATOMIC_RELAXED) & CANCEL_REQUESTED;
    }
    kj::Own<CallContextHook> addRef() override {
      return kj::addRef(*this);
    }

  private:
    RpcConnectionState& connectionState;
    QuestionId questionId;

    // Request ---------------------------------------------

    kj::Maybe<kj::Own<IncomingRpcMessage>> request;
    CapExtractorImpl requestCapExtractor;
    CapReaderContext requestCapContext;
    ObjectPointer::Reader params;

    // Response --------------------------------------------

    kj::Maybe<kj::Own<RpcServerResponse>> response;
    rpc::Return::Builder returnMessage;
    bool responseSent = false;

    // Cancellation state ----------------------------------

    enum CancellationFlags {
      CANCEL_REQUESTED = 1,
      CANCEL_ALLOWED = 2
    };

    mutable uint8_t cancellationFlags = 0;
    // When both flags are set, the cancellation process will begin.  Must be manipulated atomically
    // as it may be accessed from multiple threads.

    mutable kj::Promise<void> deferredCancellation = nullptr;
    // Cancellation operation scheduled by cancelLater().  Must only be scheduled once, from one
    // thread.

    kj::EventLoop* threadAcceptingCancellation = nullptr;
    // EventLoop for the thread that first called allowAsyncCancellation().  We store this as an
    // optimization:  if the application thread is independent from the network thread, we'd rather
    // perform the cancellation in the application thread, because otherwise we might block waiting
    // on an application promise continuation callback to finish executing, which could take
    // arbitrary time.

    // -----------------------------------------------------

    void scheduleCancel() const {
      // Arranges for the answer's asyncOp to be deleted, thus canceling all processing related to
      // this call, shortly.  We have to do it asynchronously because the caller might hold
      // arbitrary locks or might in fact be part of the task being canceled.

      deferredCancellation = threadAcceptingCancellation->evalLater([this]() {
        // Make sure we don't accidentally delete ourselves in the process of canceling, since the
        // last reference to the context may be owned by the asyncOp.
        auto self = kj::addRef(*this);

        // Extract from the answer table the promise representing the executing call.
        kj::Promise<void> asyncOp = nullptr;
        {
          auto lock = connectionState.tables.lockExclusive();
          asyncOp = kj::mv(lock->answers[questionId].asyncOp);
        }

        // Delete the promise, thereby canceling the operation.  Note that if a continuation is
        // running in another thread, this line blocks waiting for it to complete.  This is why
        // we try to schedule doCancel() on the application thread, so that it won't need to block.
        asyncOp = nullptr;

        // OK, now that we know the call isn't running in another thread, we can drop our thread
        // safety and send a return message.
        const_cast<RpcCallContext*>(this)->sendCancel();
      });
    }

    bool isFirstResponder() {
      // The first time it is called, removes self from the answer table and returns true.
      // On subsequent calls, returns false.

      if (responseSent) {
        return false;
      } else {
        responseSent = true;

        // We need to remove the `callContext` pointer -- which points back to us -- from the
        // answer table.  Or we might even be responsible for removing the entire answer table
        // entry.
        auto lock = connectionState.tables.lockExclusive();

        if (__atomic_load_n(&cancellationFlags, __ATOMIC_RELAXED) & CANCEL_REQUESTED) {
          // We are responsible for deleting the answer table entry.  Awkwardly, however, the
          // answer table may be the only thing holding a reference to the context, and we may even
          // be called from the continuation represented by answer.asyncOp.  So we have to do the
          // actual deletion asynchronously.  But we have to remove it from the table *now*, while
          // we still hold the lock, because once we send the return message the answer ID is free
          // for reuse.
          connectionState.tasks.add(connectionState.eventLoop.evalLater(
              kj::mvCapture(lock->answers[questionId],
                [](Answer<RpcCallContext>&& answer) {
                  // Just let the answer be deleted.
                })));

          // Erase from the table.
          lock->answers.erase(questionId);
        } else {
          // We just have to null out callContext.
          lock->answers[questionId].callContext = nullptr;
        }

        return true;
      }
    }
  };

  // =====================================================================================
  // Message handling

  kj::Promise<void> messageLoop() {
    return connection->receiveIncomingMessage().then(
        [this](kj::Own<IncomingRpcMessage>&& message) {
      handleMessage(kj::mv(message));
      // TODO(now):  Don't repeat messageLoop in case of exception.
      tasks.add(messageLoop());
    }, [this](kj::Exception&& exception) {
      // TODO(now):  This probably isn't right.
      connection = nullptr;
      kj::throwRecoverableException(kj::mv(exception));
    });
  }

  void handleMessage(kj::Own<IncomingRpcMessage> message) {
    auto reader = message->getBody().getAs<rpc::Message>();
    switch (reader.which()) {
      case rpc::Message::UNIMPLEMENTED:
        handleUnimplemented(reader.getUnimplemented());
        break;

      case rpc::Message::ABORT:
        handleAbort(reader.getAbort());
        break;

      case rpc::Message::CALL:
        handleCall(kj::mv(message), reader.getCall());
        break;

      case rpc::Message::RETURN:
        handleReturn(kj::mv(message), reader.getReturn());
        break;

      case rpc::Message::FINISH:
        handleFinish(reader.getFinish());
        break;

      default: {
        auto message = connection->newOutgoingMessage(
            reader.totalSizeInWords() + messageSizeHint<void>());
        message->getBody().initAs<rpc::Message>().setUnimplemented(reader);
        message->send();
        break;
      }
    }
  }

  void handleUnimplemented(const rpc::Message::Reader& message) {
    // TODO(now)
  }

  void handleAbort(const rpc::Exception::Reader& exception) {
    kj::throwRecoverableException(toException(exception));
  }

  void handleCall(kj::Own<IncomingRpcMessage>&& message, const rpc::Call::Reader& call) {
    kj::Own<const ClientHook> capability;

    auto target = call.getTarget();
    switch (target.which()) {
      case rpc::Call::Target::EXPORTED_CAP: {
        auto lock = tables.lockExclusive();  // TODO(perf):  shared?
        KJ_IF_MAYBE(exp, lock->exports.find(target.getExportedCap())) {
          capability = exp->clientHook->addRef();
        } else {
          KJ_FAIL_REQUIRE("Call target is not a current export ID.") {
            return;
          }
        }
        break;
      }

      case rpc::Call::Target::PROMISED_ANSWER: {
        auto promisedAnswer = target.getPromisedAnswer();
        kj::Own<const PipelineHook> pipeline;

        {
          auto lock = tables.lockExclusive();  // TODO(perf):  shared?
          auto& base = lock->answers[promisedAnswer.getQuestionId()];
          KJ_REQUIRE(base.active, "PromisedAnswer.questionId is not a current question.") {
            return;
          }
          KJ_IF_MAYBE(p, base.pipeline) {
            pipeline = p->get()->addRef();
          } else {
            KJ_FAIL_REQUIRE("PromisedAnswer.questionId is already finished.") {
              return;
            }
          }
        }

        KJ_IF_MAYBE(ops, toPipelineOps(promisedAnswer.getTransform())) {
          capability = pipeline->getPipelinedCap(*ops);
        } else {
          // Exception already thrown.
          return;
        }
        break;
      }

      default:
        // TODO(soon):  Handle better.
        KJ_FAIL_REQUIRE("Unknown call target type.", (uint)target.which()) {
          return;
        }
    }

    QuestionId questionId = call.getQuestionId();
    auto context = kj::refcounted<RpcCallContext>(
        *this, questionId, kj::mv(message), call.getRequest());
    auto promiseAndPipeline = capability->call(
        call.getInterfaceId(), call.getMethodId(), context->addRef());

    // No more using `call` after this point!

    {
      auto lock = tables.lockExclusive();

      auto& answer = lock->answers[questionId];

      // We don't want to overwrite an active question because the destructors for the promise and
      // pipeline could try to lock our mutex.  Of course, we did already fire off the new call
      // above, but that's OK because it won't actually ever inspect the Answer table itself, and
      // we're about to close the connection anyway.
      KJ_REQUIRE(!answer.active, "questionId is already in use") {
        return;
      }

      answer.active = true;
      answer.callContext = *context;
      answer.pipeline = kj::mv(promiseAndPipeline.pipeline);

      // Hack:  Both the success and error continuations need to use the context.  We could
      //   refcount, but both will be destroyed at the same time anyway.
      RpcCallContext* contextPtr = context;

      // TODO(cleanup):  We have the continuations return Promise<void> rather than void because
      //   this tricks the promise framework into making sure the continuations actually run
      //   without anyone waiting on them.  We should find a cleaner way to do this.
      answer.asyncOp = promiseAndPipeline.promise.then(
          kj::mvCapture(context, [this](kj::Own<RpcCallContext>&& context) -> kj::Promise<void> {
            context->sendReturn();
            return kj::READY_NOW;
          }), [this,contextPtr](kj::Exception&& exception) -> kj::Promise<void> {
            contextPtr->sendErrorReturn(kj::mv(exception));
            return kj::READY_NOW;
          });
    }
  }

  void handleReturn(kj::Own<IncomingRpcMessage>&& message, const rpc::Return::Reader& ret) {
    kj::Own<CapInjectorImpl> paramCapsToRelease;

    auto lock = tables.lockExclusive();
    KJ_IF_MAYBE(question, lock->questions.find(ret.getQuestionId())) {
      KJ_REQUIRE(question->paramCaps != nullptr, "Duplicate Return.") { return; }

      KJ_IF_MAYBE(pc, question->paramCaps) {
        // Release these later, after unlocking.
        paramCapsToRelease = kj::mv(*pc);
      } else {
        KJ_FAIL_REQUIRE("Duplicate return.") { return; }
      }

      for (ExportId retained: ret.getRetainedCaps()) {
        KJ_IF_MAYBE(exp, lock->exports.find(retained)) {
          ++exp->refcount;
        } else {
          KJ_FAIL_REQUIRE("Invalid export ID in Return.retainedCaps list.") { return; }
        }
      }

      // TODO(now):  Handle exception/cancel response

      auto response = kj::refcounted<RpcResponse>(*this, kj::mv(message), ret.getAnswer());
      question->fulfiller->fulfill(kj::mv(response));

      if (question->pipeline == nullptr) {
        lock->questions.erase(ret.getQuestionId());
      }
    } else {
      KJ_FAIL_REQUIRE("Invalid question ID in Return message.") { return; }
    }
  }

  void handleFinish(const rpc::Finish::Reader& finish) {
    kj::Maybe<kj::Own<const PipelineHook>> pipelineToRelease;

    auto lock = tables.lockExclusive();

    for (ExportId retained: finish.getRetainedCaps()) {
      KJ_IF_MAYBE(exp, lock->exports.find(retained)) {
        ++exp->refcount;
      } else {
        KJ_FAIL_REQUIRE("Invalid export ID in Return.retainedCaps list.") { return; }
      }
    }

    auto& answer = lock->answers[finish.getQuestionId()];

    // `Finish` indicates that no further pipeline requests will be made.
    pipelineToRelease = kj::mv(answer.pipeline);

    KJ_IF_MAYBE(context, answer.callContext) {
      context->requestCancel();
    } else {
      lock->answers.erase(finish.getQuestionId());
    }
  }

  // =====================================================================================

  void sendReleaseLater(ExportId importId, uint remoteRefcount) const {
    tasks.add(eventLoop.evalLater([this,importId,remoteRefcount]() {
      auto message = connection->newOutgoingMessage(messageSizeHint<rpc::Release>());
      rpc::Release::Builder builder = message->getBody().initAs<rpc::Message>().initRelease();
      builder.setId(importId);
      builder.setReferenceCount(remoteRefcount);
      message->send();
    }));
  }
};

}  // namespace

class RpcSystemBase::Impl {
public:
  Impl(VatNetworkBase& network, SturdyRefRestorerBase& restorer, const kj::EventLoop& eventLoop)
      : network(network), restorer(restorer), eventLoop(eventLoop) {}

  Capability::Client connect(_::StructReader reader) {
    // TODO(now)
  }

private:
  VatNetworkBase& network;
  SturdyRefRestorerBase& restorer;
  const kj::EventLoop& eventLoop;
};

RpcSystemBase::RpcSystemBase(VatNetworkBase& network, SturdyRefRestorerBase& restorer,
                             const kj::EventLoop& eventLoop)
    : impl(kj::heap<Impl>(network, restorer, eventLoop)) {}
RpcSystemBase::~RpcSystemBase() noexcept(false) {}

Capability::Client RpcSystemBase::baseConnect(_::StructReader reader) {
  return impl->connect(reader);
}

}  // namespace _ (private)
}  // namespace capnp
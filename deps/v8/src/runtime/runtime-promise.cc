// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/v8-microtask-dispatch.h"
#include "src/api/api-inl.h"
#include "src/debug/debug.h"
#include "src/execution/arguments-inl.h"
#include "src/execution/execution.h"
#include "src/execution/microtask-queue.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/microtask-inl.h"
#include "src/objects/promise-inl.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_PromiseRejectEventFromStack) {
  DCHECK_EQ(2, args.length());
  HandleScope scope(isolate);
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<Object> value = args.at(1);

  isolate->RunAllPromiseHooks(PromiseHookType::kResolve, promise,
                              isolate->factory()->undefined_value());
  isolate->debug()->OnPromiseReject(promise, value);

  // Report only if we don't actually have a handler.
  if (!promise->has_handler()) {
    isolate->ReportPromiseReject(promise, value,
                                 v8::kPromiseRejectWithNoHandler);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PromiseRejectAfterResolved) {
  DCHECK_EQ(2, args.length());
  HandleScope scope(isolate);
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<Object> reason = args.at(1);
  isolate->ReportPromiseReject(promise, reason,
                               v8::kPromiseRejectAfterResolved);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PromiseResolveAfterResolved) {
  DCHECK_EQ(2, args.length());
  HandleScope scope(isolate);
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<Object> resolution = args.at(1);
  isolate->ReportPromiseReject(promise, resolution,
                               v8::kPromiseResolveAfterResolved);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PromiseRevokeReject) {
  DCHECK_EQ(1, args.length());
  HandleScope scope(isolate);
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  // At this point, no revocation has been issued before
  CHECK(!promise->has_handler());
  isolate->ReportPromiseReject(promise, DirectHandle<Object>(),
                               v8::kPromiseHandlerAddedAfterReject);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_EnqueueMicrotask) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSFunction> function = args.at<JSFunction>(0);

  DirectHandle<CallableTask> microtask = isolate->factory()->NewCallableTask(
      function, direct_handle(function->native_context(), isolate));
  MicrotaskQueue* microtask_queue =
      function->native_context()->microtask_queue();
  if (microtask_queue) microtask_queue->EnqueueMicrotask(*microtask);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PerformMicrotaskCheckpoint) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  MicrotasksScope::PerformCheckpoint(reinterpret_cast<v8::Isolate*>(isolate));
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RunMicrotaskCallback) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Tagged<Object> microtask_callback = args[0];
  Tagged<Object> microtask_data = args[1];
  MicrotaskCallback callback =
      ToCData<MicrotaskCallback, kMicrotaskCallbackTag>(isolate,
                                                        microtask_callback);
  void* data =
      ToCData<void*, kMicrotaskCallbackDataTag>(isolate, microtask_data);
  callback(data);
  RETURN_FAILURE_IF_EXCEPTION(isolate);
  return ReadOnlyRoots(isolate).undefined_value();
}

// Prototype (Qualia): hand promise reaction jobs that carry continuation-
// preserved embedder data to the embedder, so it can run them on another
// stack (node-fibers). See include/v8-microtask-dispatch.h.
}  // namespace internal

struct DispatchedMicrotask {
  internal::MicrotaskQueue* queue;
};

namespace internal {

namespace {
v8::MicrotaskDispatchCallback g_microtask_dispatch_callback = nullptr;
void* g_microtask_dispatch_data = nullptr;
Isolate* g_microtask_dispatch_isolate = nullptr;
// Set just before a private single-job queue runs, so that job runs inline
// instead of being dispatched again.
bool g_run_next_dispatchable_inline = false;
}  // namespace

// Runs the one job in |queue| on the current stack, with the bookkeeping
// MicrotaskQueue::RunMicrotasks does around a drain (the job sets its own CPED;
// the caller's CPED is restored afterwards). Bypasses RunMicrotasks itself so
// the default queue's suppression and running state aren't touched while the
// job may be parked on a coroutine.
static void RunPrivateMicrotaskQueue(Isolate* isolate, MicrotaskQueue* queue) {
  HandleScope handle_scope(isolate);
#ifdef V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
  DirectHandle<Object> outer_embedder_data(
      isolate->isolate_data()->continuation_preserved_embedder_data(), isolate);
  isolate->isolate_data()->set_continuation_preserved_embedder_data(
      ReadOnlyRoots(isolate).undefined_value());
#endif  // V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
  {
    HandleScopeImplementer::EnteredContextRewindScope rewind_scope(
        isolate->handle_scope_implementer());
    g_run_next_dispatchable_inline = true;
    MaybeDirectHandle<Object> result =
        Execution::TryRunMicrotasks(isolate, queue);
    USE(result);
    // Normally consumed by Runtime_DispatchMicrotask before the job starts.
    g_run_next_dispatchable_inline = false;
  }
#ifdef V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
  isolate->isolate_data()->set_continuation_preserved_embedder_data(
      *outer_embedder_data);
#endif  // V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
  if (isolate->is_execution_terminating()) {
    isolate->OnTerminationDuringRunMicrotasks();
  }
}

RUNTIME_FUNCTION(Runtime_DispatchMicrotask) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  if (g_run_next_dispatchable_inline) {
    g_run_next_dispatchable_inline = false;
    return ReadOnlyRoots(isolate).false_value();
  }
  if (g_microtask_dispatch_callback == nullptr ||
      g_microtask_dispatch_isolate != isolate) {
    return ReadOnlyRoots(isolate).false_value();
  }
  DirectHandle<Microtask> microtask = args.at<Microtask>(0);
  // A private single-job queue keeps the job alive (queues are GC roots) and
  // lets the job run through the RunMicrotasks builtin.
  std::unique_ptr<MicrotaskQueue> queue = MicrotaskQueue::New(isolate);
  queue->EnqueueMicrotask(*microtask);
  v8::DispatchedMicrotask* task = new v8::DispatchedMicrotask{queue.get()};
  bool taken = g_microtask_dispatch_callback(
      reinterpret_cast<v8::Isolate*>(isolate), task, g_microtask_dispatch_data);
  if (taken) {
    queue.release();  // Owned (and possibly already freed) by the embedder.
  } else {
    delete task;
  }
  RETURN_FAILURE_IF_EXCEPTION(isolate);
  return isolate->heap()->ToBoolean(taken);
}

}  // namespace internal

void SetMicrotaskDispatchCallback(Isolate* v8_isolate,
                                  MicrotaskDispatchCallback callback,
                                  void* data) {
  internal::g_microtask_dispatch_callback = callback;
  internal::g_microtask_dispatch_data = data;
  internal::g_microtask_dispatch_isolate =
      reinterpret_cast<internal::Isolate*>(v8_isolate);
}

void RunDispatchedMicrotask(Isolate* v8_isolate, DispatchedMicrotask* task) {
  internal::Isolate* isolate = reinterpret_cast<internal::Isolate*>(v8_isolate);
  std::unique_ptr<internal::MicrotaskQueue> queue(task->queue);
  delete task;
  internal::RunPrivateMicrotaskQueue(isolate, queue.get());
}

bool RunNextDispatchableMicrotask(Isolate* v8_isolate) {
#ifdef V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
  internal::Isolate* isolate = reinterpret_cast<internal::Isolate*>(v8_isolate);
  internal::MicrotaskQueue* queue = isolate->default_microtask_queue();
  if (queue == nullptr || queue->size() == 0) return false;
  internal::HandleScope handle_scope(isolate);
  internal::Tagged<internal::Microtask> front = queue->get(0);
  if (!internal::IsPromiseFulfillReactionJobTask(front) &&
      !internal::IsPromiseRejectReactionJobTask(front)) {
    return false;
  }
  if (internal::IsUndefined(front->continuation_preserved_embedder_data(),
                            isolate)) {
    return false;
  }
  internal::DirectHandle<internal::Microtask> microtask(front, isolate);
  // Take it off the front of the queue the way the RunMicrotasks builtin does
  // (that loop re-reads start and size before every job, so the drain this is
  // nested in carries on with the job after it).
  internal::Address base = reinterpret_cast<internal::Address>(queue);
  intptr_t* start = reinterpret_cast<intptr_t*>(
      base + internal::MicrotaskQueue::kStartOffset);
  intptr_t* size = reinterpret_cast<intptr_t*>(
      base + internal::MicrotaskQueue::kSizeOffset);
  *start = (*start + 1) % queue->capacity();
  *size -= 1;
  std::unique_ptr<internal::MicrotaskQueue> private_queue =
      internal::MicrotaskQueue::New(isolate);
  private_queue->EnqueueMicrotask(*microtask);
  internal::RunPrivateMicrotaskQueue(isolate, private_queue.get());
  return true;
#else
  return false;
#endif  // V8_ENABLE_CONTINUATION_PRESERVED_EMBEDDER_DATA
}

}  // namespace v8

extern "C" {
void v8_qualia_SetMicrotaskDispatchCallback(
    v8::Isolate* isolate, v8::MicrotaskDispatchCallback callback, void* data) {
  v8::SetMicrotaskDispatchCallback(isolate, callback, data);
}

void v8_qualia_RunDispatchedMicrotask(v8::Isolate* isolate,
                                      v8::DispatchedMicrotask* task) {
  v8::RunDispatchedMicrotask(isolate, task);
}

bool v8_qualia_RunNextDispatchableMicrotask(v8::Isolate* isolate) {
  return v8::RunNextDispatchableMicrotask(isolate);
}
}

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_PromiseHookInit) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<Object> parent = args.at(1);
  isolate->RunPromiseHook(PromiseHookType::kInit, promise, parent);
  RETURN_FAILURE_IF_EXCEPTION(isolate);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PromiseHookBefore) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSReceiver> promise = args.at<JSReceiver>(0);
  if (IsJSPromise(*promise)) {
    isolate->OnPromiseBefore(Cast<JSPromise>(promise));
    RETURN_FAILURE_IF_EXCEPTION(isolate);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PromiseHookAfter) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSReceiver> promise = args.at<JSReceiver>(0);
  if (IsJSPromise(*promise)) {
    isolate->OnPromiseAfter(Cast<JSPromise>(promise));
    RETURN_FAILURE_IF_EXCEPTION(isolate);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RejectPromise) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<Object> reason = args.at(1);
  DirectHandle<Boolean> debug_event = args.at<Boolean>(2);
  return *JSPromise::Reject(promise, reason,
                            Object::BooleanValue(*debug_event, isolate));
}

RUNTIME_FUNCTION(Runtime_ResolvePromise) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<Object> resolution = args.at(1);
  DirectHandle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, result,
                                     JSPromise::Resolve(promise, resolution));
  return *result;
}

// A helper function to be called when constructing AggregateError objects. This
// takes care of the Error-related construction, e.g., stack traces.
RUNTIME_FUNCTION(Runtime_ConstructAggregateErrorHelper) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  DirectHandle<JSFunction> target = args.at<JSFunction>(0);
  DirectHandle<Object> new_target = args.at(1);
  DirectHandle<Object> message = args.at(2);
  DirectHandle<Object> options = args.at(3);

  DCHECK_EQ(*target, *isolate->aggregate_error_function());

  DirectHandle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result,
      ErrorUtils::Construct(isolate, target, new_target, message, options));
  return *result;
}

// A helper function to be called when constructing AggregateError objects. This
// takes care of the Error-related construction, e.g., stack traces.
RUNTIME_FUNCTION(Runtime_ConstructInternalAggregateErrorHelper) {
  HandleScope scope(isolate);
  DCHECK_GE(args.length(), 1);
  int message_template_index = args.smi_value_at(0);

  constexpr int kMaxMessageArgs = 3;
  DirectHandle<Object> message_args[kMaxMessageArgs];
  int num_message_args = 0;

  while (num_message_args < kMaxMessageArgs &&
         args.length() > num_message_args + 1) {
    message_args[num_message_args] = args.at(num_message_args + 1);
  }

  DirectHandle<Object> options =
      args.length() >= 5 ? args.at(4) : isolate->factory()->undefined_value();

  DirectHandle<Object> message_string =
      MessageFormatter::Format(isolate, MessageTemplate(message_template_index),
                               base::VectorOf(message_args, num_message_args));

  DirectHandle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result,
      ErrorUtils::Construct(isolate, isolate->aggregate_error_function(),
                            isolate->aggregate_error_function(), message_string,
                            options));
  return *result;
}

RUNTIME_FUNCTION(Runtime_ConstructSuppressedError) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  DirectHandle<JSFunction> target = args.at<JSFunction>(0);
  DirectHandle<Object> new_target = args.at(1);
  DirectHandle<Object> message = args.at(2);

  DCHECK_EQ(*target, *isolate->suppressed_error_function());

  DirectHandle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result,
      ErrorUtils::Construct(isolate, target, new_target, message,
                            isolate->factory()->undefined_value()));
  return *result;
}

}  // namespace internal
}  // namespace v8

// Copyright 2026 Qualia Labs. Prototype, not upstream V8.
//
// Lets an embedder run a promise reaction job somewhere other than inline in
// the microtask loop, e.g. on a coroutine (node-fibers) stack. Only reaction
// jobs whose continuation-preserved embedder data (CPED, captured when the
// reaction was registered) is not undefined are offered to the embedder.

#ifndef INCLUDE_V8_MICROTASK_DISPATCH_H_
#define INCLUDE_V8_MICROTASK_DISPATCH_H_

#include "v8config.h"  // NOLINT(build/include_directory)

namespace v8 {

class Isolate;

// Opaque handle to a single promise reaction job taken out of the microtask
// queue.
struct DispatchedMicrotask;

// Called by the microtask loop for a promise reaction job with non-undefined
// CPED. Return true to take ownership of |task|; the embedder must then call
// RunDispatchedMicrotask(isolate, task) exactly once (before the next
// microtask runs, or later). Return false to have V8 run the job inline as
// usual; V8 then frees |task|.
using MicrotaskDispatchCallback = bool (*)(Isolate* isolate,
                                           DispatchedMicrotask* task,
                                           void* data);

V8_EXPORT void SetMicrotaskDispatchCallback(Isolate* isolate,
                                            MicrotaskDispatchCallback callback,
                                            void* data);

// Runs |task| on the current stack with the usual microtask bookkeeping
// (context, CPED, promise hooks, exception reporting) and frees it.
V8_EXPORT void RunDispatchedMicrotask(Isolate* isolate,
                                      DispatchedMicrotask* task);

// If the next job in the isolate's default microtask queue is a promise
// reaction job with non-undefined CPED, takes it off the queue and runs it on
// the current stack, as RunDispatchedMicrotask does, and returns true.
// Otherwise returns false and leaves the queue alone. Lets an embedder that is
// running a dispatched job keep running the dispatchable jobs right behind it
// without switching stacks for each one. Only call it while the job that was
// dispatched is still nested inside the microtask loop, so the order of jobs
// is unchanged.
V8_EXPORT bool RunNextDispatchableMicrotask(Isolate* isolate);

}  // namespace v8

// Unmangled entry points, so an addon can find them with dlsym() and still
// load (with the feature off) on a node binary without this patch.
extern "C" {
V8_EXPORT void v8_qualia_SetMicrotaskDispatchCallback(
    v8::Isolate* isolate, v8::MicrotaskDispatchCallback callback, void* data);
V8_EXPORT void v8_qualia_RunDispatchedMicrotask(v8::Isolate* isolate,
                                                v8::DispatchedMicrotask* task);
V8_EXPORT bool v8_qualia_RunNextDispatchableMicrotask(v8::Isolate* isolate);
}

#endif  // INCLUDE_V8_MICROTASK_DISPATCH_H_

// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/platform/platform.h"

namespace v8 {
namespace base {

namespace {

// A pointer to current thread's stack beginning.
thread_local void* thread_stack_start = nullptr;

// Qualia: embedders that switch stacks on one OS thread (node-fibers with
// coroutines) tell us the start of the stack that is currently executing. When
// set, it replaces the OS thread's stack start everywhere GetStackStart() is
// used: stack limits, IsOnStack() and the conservative stack scan performed by
// cppgc, which would otherwise walk from a coroutine stack up to the thread's
// stack start through unmapped memory.
thread_local void* thread_stack_start_override = nullptr;

}  // namespace

// static
Stack::StackSlot Stack::GetStackStartUnchecked() {
  if (thread_stack_start_override) {
    return thread_stack_start_override;
  }
  if (!thread_stack_start) {
    thread_stack_start = ObtainCurrentThreadStackStart();
  }
  return thread_stack_start;
}

// static
Stack::StackSlot Stack::GetStackStart() { return GetStackStartUnchecked(); }

}  // namespace base
}  // namespace v8

// Qualia: C entry point for node-fibers (resolved with dlsym, so fibers still
// loads on a node without this patch). Pass nullptr when switching back to the
// OS thread's own stack.
extern "C" __attribute__((visibility("default"))) void
v8_qualia_set_thread_stack_start(void* stack_start) {
  v8::base::thread_stack_start_override = stack_start;
}

namespace v8 {
namespace base {

// static
int OS::GetCurrentThreadId() {
  static thread_local int id = GetCurrentThreadIdInternal();
  return id;
}

}  // namespace base
}  // namespace v8

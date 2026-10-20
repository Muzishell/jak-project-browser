/*!
 * @file libco_pthread.cpp
 * libco (third-party/libco/libco.h) on top of pthreads, for the Emscripten build only.
 *
 * Wasm has no way to switch stacks without Asyncify or JSPI, so each coroutine is a real thread
 * with a baton: co_switch wakes the target and blocks the caller, so exactly one of them runs at a
 * time, like the native coroutines. Only IOP_Kernel uses libco: one thread per IOP thread.
 */

#include <pthread.h>

#include <condition_variable>
#include <mutex>

#include "libco.h"

#include "common/util/Assert.h"

namespace {
// Not std::binary_semaphore: Emscripten's libc++ has no futex for atomic waits and acquire polls
// with sleeps, which a browser worker rounds up to the OS timer tick (~15.6 ms on Windows) per IOP
// thread switch. A condition variable waits on a futex that release wakes at once.
class Baton {
 public:
  void release() {
    {
      std::lock_guard lock(m_mutex);
      m_ready = true;
    }
    m_cv.notify_one();
  }

  void acquire() {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_ready; });
    m_ready = false;
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_ready = false;
};

struct Coroutine {
  void (*entry)() = nullptr;
  unsigned stack_size = 0;
  bool started = false;
  Baton baton;  // released when this coroutine may run
};

thread_local Coroutine* t_current = nullptr;

void* coroutine_main(void* arg) {
  auto* self = (Coroutine*)arg;
  t_current = self;
  self->baton.acquire();
  self->entry();
  ASSERT_MSG(false, "libco coroutines must not return");
  return nullptr;
}
}  // namespace

extern "C" {

cothread_t co_active() {
  if (!t_current) {
    // the thread that first uses libco becomes a coroutine itself
    t_current = new Coroutine();
    t_current->started = true;
  }
  return t_current;
}

cothread_t co_derive(void*, unsigned int, void (*)(void)) {
  ASSERT_MSG(false, "co_derive is not supported by the pthread libco");
  return nullptr;
}

cothread_t co_create(unsigned int stack_size, void (*entry)(void)) {
  auto* co = new Coroutine();
  co->entry = entry;
  co->stack_size = stack_size;
  return co;
}

void co_delete(cothread_t) {
  // IOP threads never return and only go away at shutdown; leave the thread blocked.
}

void co_switch(cothread_t target) {
  auto* from = (Coroutine*)co_active();
  auto* to = (Coroutine*)target;
  if (to == from) {
    return;
  }
  if (!to->started) {
    to->started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (to->stack_size) {
      pthread_attr_setstacksize(&attr, to->stack_size);
    }
    pthread_t thread;
    ASSERT(pthread_create(&thread, &attr, coroutine_main, to) == 0);
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
  }
  to->baton.release();
  from->baton.acquire();
}

int co_serializable() {
  return 0;
}
}

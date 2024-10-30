#include "fd.h"
#include <Rcpp.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <memory>
#include "tinycthread.h"
#include "later.h"
#include "callback_registry_table.h"

#define THREAD_RETURN(xc) { thread_active->store(false); return xc; }

extern CallbackRegistryTable callbackRegistryTable;

class ThreadArgs {
public:
  ThreadArgs(
    int num_fds = 0,
    struct pollfd *fds = nullptr,
    double timeout = 0,
    int loop = 0
  )
    : flag(std::make_shared<std::atomic<bool>>(false)),
      fds(initializeFds(num_fds, fds)),
      results(std::unique_ptr<std::vector<int>>(new std::vector<int>(num_fds))),
      callback(nullptr),
      func(nullptr),
      timeout(createTimestamp(timeout)),
      num_fds(num_fds),
      loop(loop) {}

  std::shared_ptr<std::atomic<bool>> flag;
  std::unique_ptr<std::vector<struct pollfd>> fds;
  std::unique_ptr<std::vector<int>> results;
  std::unique_ptr<Rcpp::Function> callback;
  std::function<void (int *)> func;
  Timestamp timeout;
  int num_fds;
  int loop;

private:
  static std::unique_ptr<std::vector<struct pollfd>> initializeFds(int num_fds, struct pollfd *fds) {
    std::unique_ptr<std::vector<struct pollfd>> pollfds(new std::vector<struct pollfd>());
    if (fds != nullptr) {
      pollfds->reserve(num_fds);
      for (int i = 0; i < num_fds; i++) {
        pollfds->push_back(fds[i]);
      }
    }
    return pollfds;
  }
  static Timestamp createTimestamp(double timeout) {
    if (timeout == R_PosInf) {
      timeout = 3e10; // "1000 years ought to be enough for anybody" --Bill Gates
    } else if (timeout < 0) {
      timeout = 1; // curl_multi_timeout() uses -1 to denote a default we set at 1s
    }
    return Timestamp(timeout);
  }

};

// special condition variable class for later_fd()
class Cv {
  tct_mtx_t _m;
  tct_cnd_t _c;
  bool _busy; // condition

public:
  Cv() {
    if (tct_mtx_init(&_m, tct_mtx_plain) != tct_thrd_success)
      throw std::runtime_error("Mutex failed to initialize");
    if (tct_cnd_init(&_c) != tct_thrd_success)
      throw std::runtime_error("Condition variable failed to initialize");
  }
  bool lock() {
    return tct_mtx_lock(&_m) != tct_thrd_success;
  }
  bool unlock() {
    return tct_mtx_unlock(&_m) != tct_thrd_success;
  }
  bool signal() {
    return tct_cnd_signal(&_c) != tct_thrd_success;
  }
  bool wait() {
    return tct_cnd_wait(&_c, &_m) != tct_thrd_success;
  }
  bool busy() {
    return _busy;
  }
  void busy(bool value) {
    _busy = value;
  }
  void destroy() {
    tct_cnd_destroy(&_c);
    tct_mtx_destroy(&_m);
  }

};

// for persistent wait thread
static std::shared_ptr<std::atomic<bool>> thread_active = std::make_shared<std::atomic<bool>>(false);
static std::unique_ptr<std::shared_ptr<ThreadArgs>> thread_args;
static Cv cv;

// accessor for init.c
extern "C" void later_exiting(void) {
  if (thread_active->load()) {
    thread_active->store(false); // atomic so can be called outside lock
    cv.lock();
    if (cv.busy())
      (*thread_args)->flag->store(false);
    cv.signal();
    cv.unlock();
  }
  cv.destroy(); // must be called eplicitly here to ensure this happens last
}

// callback executed on main thread
static void later_callback(void *arg) {

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(static_cast<std::shared_ptr<ThreadArgs>*>(arg));
  std::shared_ptr<ThreadArgs> args = *argsptr;
  const bool flag = args->flag->load();
  args->flag->store(true);
  if (flag)
    return;
  if (args->func != nullptr) {
    args->func(args->results->data());
  } else {
    Rcpp::LogicalVector results = Rcpp::wrap(*args->results);
    (*args->callback)(results);
  }

}

// CONSIDER: if necessary to add method for HANDLES on Windows. Would be different code to SOCKETs.
static int wait_on_fds(std::shared_ptr<ThreadArgs> args) {

  // poll() whilst checking for cancellation at intervals

  int ready = -1; // initialized at -1 to ensure it runs at least once
  while (true) {
    double waitFor_ms = args->timeout.diff_secs(Timestamp()) * 1000;
    if (waitFor_ms <= 0) {
      if (!ready) break; // only breaks after the first time
      waitFor_ms = 0;
    } else if (waitFor_ms > LATER_POLL_INTERVAL) {
      waitFor_ms = LATER_POLL_INTERVAL;
    }
    ready = LATER_POLL_FUNC(args->fds->data(), args->num_fds, static_cast<int>(waitFor_ms));
    if (args->flag->load()) return 1;
    if (ready) break;
  }

  // store pollfd revents in args->results for use by callback

  if (ready > 0) {
    for (int i = 0; i < args->num_fds; i++) {
      (*args->results)[i] = (*args->fds)[i].revents == 0 ? 0 : (*args->fds)[i].revents & (POLLIN | POLLOUT) ? 1: NA_INTEGER;
    }
  } else if (ready < 0) {
    std::fill(args->results->begin(), args->results->end(), NA_INTEGER);
  }

  return 0;

}

static int wait_thread_single(void *arg) {

  tct_thrd_detach(tct_thrd_current());

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(static_cast<std::shared_ptr<ThreadArgs>*>(arg));
  std::shared_ptr<ThreadArgs> args = *argsptr;

  if (wait_on_fds(args))
    return 1;

  callbackRegistryTable.scheduleCallback(later_callback, static_cast<void *>(argsptr.release()), 0, args->loop);

  return 0;

}

static int wait_thread_persistent(void *arg) {

  tct_thrd_detach(tct_thrd_current());
  thread_active->store(true); // atomic allows update prior to acquiring lock

  if (cv.lock()) THREAD_RETURN(1);
  if (cv.signal()) { cv.unlock(); THREAD_RETURN(1); } // signal to sync with main thread
  cv.busy(false);
  while (!cv.busy()) {
    if (cv.wait()) { cv.unlock(); THREAD_RETURN(1); }
  }
  if (cv.unlock()) THREAD_RETURN(1);

  while (1) {

    if (!thread_active->load()) return(1); // set by later_exiting() on unload

    std::shared_ptr<ThreadArgs> args = *thread_args;

    if (wait_on_fds(args) == 0) // only callback if not cancelled
      callbackRegistryTable.scheduleCallback(later_callback, static_cast<void *>(thread_args.release()), 0, args->loop);

    if (cv.lock()) THREAD_RETURN(1);
    cv.busy(false);
    while (!cv.busy()) {
      if (cv.wait()) { cv.unlock(); THREAD_RETURN(1); }
    }
    if (cv.unlock()) THREAD_RETURN(1);

  }

  return 0;

}

static int execLater_launch_thread(std::shared_ptr<ThreadArgs> args) {

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(new std::shared_ptr<ThreadArgs>(args));

  if (!thread_active->load()) {

    tct_thrd_t thr;
    if (tct_thrd_create(&thr, &wait_thread_persistent, NULL) != tct_thrd_success)
      return 1;

    if (cv.lock()) return 1;
    while (!thread_active->load()) { // not the condition, but thread will update this
      if (cv.wait()) { cv.unlock(); return 1; } // wait for signal from thread
    }
    if (cv.unlock()) return 1;

  }

  do {

    if (cv.lock()) return 1;
    if (cv.busy()) { cv.unlock(); break; } // busy so create new single thread

    cv.busy(true);
    thread_args = std::move(argsptr);
    if (cv.signal()) { cv.unlock(); return 1; }
    if (cv.unlock()) return 1;
    return 0;

  } while (0);

  tct_thrd_t thr;

  return tct_thrd_create(&thr, &wait_thread_single, static_cast<void *>(argsptr.release())) != tct_thrd_success;

}

static SEXP execLater_fd_impl(Rcpp::Function callback, int num_fds, struct pollfd *fds, double timeout, int loop_id) {

  std::shared_ptr<ThreadArgs> args = std::make_shared<ThreadArgs>(num_fds, fds, timeout, loop_id);
  args->callback = std::unique_ptr<Rcpp::Function>(new Rcpp::Function(callback));

  if (execLater_launch_thread(args))
    Rcpp::stop("later_fd() wait failed");

  Rcpp::XPtr<std::shared_ptr<std::atomic<bool>>> xptr(new std::shared_ptr<std::atomic<bool>>(args->flag), true);
  return xptr;

}

// native version
static int execLater_fd_impl(void (*func)(int *, void *), void *data, int num_fds, struct pollfd *fds, double timeout, int loop_id) {

  std::shared_ptr<ThreadArgs> args = std::make_shared<ThreadArgs>(num_fds, fds, timeout, loop_id);
  args->func = std::bind(func, std::placeholders::_1, data);

  return execLater_launch_thread(args);

}

// [[Rcpp::export]]
Rcpp::RObject execLater_fd(Rcpp::Function callback, Rcpp::IntegerVector readfds, Rcpp::IntegerVector writefds,
                           Rcpp::IntegerVector exceptfds, Rcpp::NumericVector timeoutSecs, Rcpp::IntegerVector loop_id) {

  const int rfds = static_cast<int>(readfds.size());
  const int wfds = static_cast<int>(writefds.size());
  const int efds = static_cast<int>(exceptfds.size());
  const int num_fds = rfds + wfds + efds;
  if (num_fds == 0)
    Rcpp::stop("No file descriptors supplied");

  double timeout = timeoutSecs[0];
  const int loop = loop_id[0];

  std::vector<struct pollfd> pollfds;
  pollfds.reserve(num_fds);
  struct pollfd pfd;

  for (int i = 0; i < rfds; i++) {
    pfd.fd = readfds[i];
    pfd.events = POLLIN;
    pfd.revents = 0;
    pollfds.push_back(pfd);
  }
  for (int i = 0; i < wfds; i++) {
    pfd.fd = writefds[i];
    pfd.events = POLLOUT;
    pfd.revents = 0;
    pollfds.push_back(pfd);
  }
  for (int i = 0; i < efds; i++) {
    pfd.fd = exceptfds[i];
    pfd.events = 0;
    pfd.revents = 0;
    pollfds.push_back(pfd);
  }

  return execLater_fd_impl(callback, num_fds, pollfds.data(), timeout, loop);

}

// [[Rcpp::export]]
Rcpp::LogicalVector fd_cancel(Rcpp::RObject xptr) {

  if (TYPEOF(xptr) != EXTPTRSXP || R_ExternalPtrAddr(xptr) == NULL)
    Rcpp::stop("Invalid external pointer");

  Rcpp::XPtr<std::shared_ptr<std::atomic<bool>>> flag(xptr);

  if ((*flag)->load())
    return false;

  (*flag)->store(true);
  return true;

}

// Schedules a C function that takes a pointer to an integer array (provided by
// this function when calling back) and a void * argument, to execute on file
// descriptor readiness. Returns 0 upon success and 1 on failure. NOTE: this is
// different to execLaterNative2() which returns 0 on failure.
extern "C" int execLaterFdNative(void (*func)(int *, void *), void *data, int num_fds, struct pollfd *fds, double timeoutSecs, int loop_id) {
  ensureInitialized();
  return execLater_fd_impl(func, data, num_fds, fds, timeoutSecs, loop_id);
}

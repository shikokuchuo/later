#include "fd.h"
#include <Rcpp.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <memory>
#include "tinycthread.h"
#include "later.h"
#include "callback_registry_table.h"

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

// for persistent wait thread
tct_mtx_t mutex;
tct_cnd_t condvar;
bool thread_busy = false; // condition
std::unique_ptr<std::shared_ptr<ThreadArgs>> thread_args;
static std::atomic<bool> thread_active = false;

// accessors for init.c
extern "C" int later_thread_active(void) {
  return thread_active.load();
}

extern "C" void later_set_exiting(void) {
  thread_active.store(false);
}

extern "C" void cancel_busy_thread(void) {
  if (thread_busy)
    (*thread_args)->flag->store(false);
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

static int wait_thread_ephemeral(void *arg) {

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
  thread_active.store(true);

  if (tct_mtx_lock(&mutex) != tct_thrd_success)
    goto exit;
  if (tct_cnd_signal(&condvar) != tct_thrd_success)
    goto unlock_and_exit;
  thread_busy = false;
  while (!thread_busy) {
    if (tct_cnd_wait(&condvar, &mutex) != tct_thrd_success)
      goto unlock_and_exit;
  }
  if (tct_mtx_unlock(&mutex) != tct_thrd_success)
    goto exit;

  while (1) {

    // set to false by later_exiting() on unload
    if (!thread_active.load())
      goto exit;

    std::shared_ptr<ThreadArgs> args = *thread_args;

    if (wait_on_fds(args))
      goto exit;

    callbackRegistryTable.scheduleCallback(later_callback, static_cast<void *>(thread_args.release()), 0, args->loop);

    if (tct_mtx_lock(&mutex) != tct_thrd_success)
      goto exit;
    thread_busy = false;
    while (!thread_busy) {
      if (tct_cnd_wait(&condvar, &mutex) != tct_thrd_success)
        goto unlock_and_exit;
    }
    if (tct_mtx_unlock(&mutex) != tct_thrd_success)
      goto exit;

  }

  return 0;

  unlock_and_exit:
  tct_mtx_unlock(&mutex);
  exit:
  thread_active.store(false);
  return 1;

}

static int execLater_launch_thread(std::shared_ptr<ThreadArgs> args) {

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(new std::shared_ptr<ThreadArgs>(args));

  if (!thread_active.load()) {
    if (tct_mtx_init(&mutex, tct_mtx_plain) != tct_thrd_success)
      return 1;

    if (tct_cnd_init(&condvar) != tct_thrd_success) {
      tct_mtx_destroy(&mutex);
      return 1;
    }

    tct_thrd_t thr;
    tct_thrd_create(&thr, &wait_thread_persistent, NULL);

    if (tct_mtx_lock(&mutex) != tct_thrd_success)
      return 1;
    while (!thread_active.load()) {
      if (tct_cnd_wait(&condvar, &mutex) != tct_thrd_success) {
        tct_mtx_unlock(&mutex);
        return 1;
      }
    }
    if (tct_mtx_unlock(&mutex) != tct_thrd_success)
      return 1;
  }

  do {
    if (tct_mtx_lock(&mutex) != tct_thrd_success)
      return 1;
    if (thread_busy) {
      tct_mtx_unlock(&mutex);
      break;
    }
    thread_busy = true;
    thread_args = std::move(argsptr);
    if (tct_cnd_signal(&condvar) != tct_thrd_success) {
      tct_mtx_unlock(&mutex);
      return 1;
    }
    if (tct_mtx_unlock(&mutex) != tct_thrd_success)
      return 1;

    return 0;
  } while (0);

  tct_thrd_t thr;

  return tct_thrd_create(&thr, &wait_thread_ephemeral, static_cast<void *>(argsptr.release())) != tct_thrd_success;

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

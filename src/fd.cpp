#ifdef _WIN32
#ifndef FD_SETSIZE
#define FD_SETSIZE 2048  // set max fds - only for select() fallback on R <= 4.1
#endif
#include <winsock2.h>
#else
#include <poll.h>
#endif
#include <Rcpp.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <memory>
#include "tinycthread.h"
#include "later.h"

#define LATER_POLL_INTERVAL 1024
#ifdef _WIN32
#define LATER_POLL_FUNC WSAPoll
#else
#define LATER_POLL_FUNC poll
#endif

typedef struct ThreadArgs_s {
  std::shared_ptr<std::atomic<bool>> flag;
  std::unique_ptr<Rcpp::Function> callback;
  std::unique_ptr<std::vector<int>> fds;
  std::unique_ptr<std::vector<int>> results;
  Timestamp timeout;
  int rfds, wfds, efds, num_fds;
  int loop;
} ThreadArgs;

static void later_callback(void *arg) {

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(static_cast<std::shared_ptr<ThreadArgs>*>(arg));
  std::shared_ptr<ThreadArgs> args = *argsptr;
  const bool flag = args->flag->load();
  args->flag->store(true);
  if (!flag) {
    Rcpp::LogicalVector results = Rcpp::wrap(*args->results);
    (*args->callback)(results);
  }

}

// CONSIDER: if necessary to add method for HANDLES on Windows. Would be different code to SOCKETs.
// TODO: implement re-usable background thread.

#ifdef POLLIN // all platforms except R <= 4.1 on Windows

static int wait_thread(void *arg) {

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(static_cast<std::shared_ptr<ThreadArgs>*>(arg));
  std::shared_ptr<ThreadArgs> args = *argsptr;

  // setup pollfd structs from args->fds
  std::vector<struct pollfd> pollfds;
  pollfds.reserve(args->num_fds);
  struct pollfd pfd;

  for (int i = 0; i < args->rfds; i++) {
    pfd.fd = (*args->fds)[i];
    pfd.events = POLLIN;
    pfd.revents = 0;
    pollfds.push_back(pfd);
  }
  for (int i = args->rfds; i < (args->rfds + args->wfds); i++) {
    pfd.fd = (*args->fds)[i];
    pfd.events = POLLOUT;
    pfd.revents = 0;
    pollfds.push_back(pfd);
  }
  for (int i = args->rfds + args->wfds; i < args->num_fds; i++) {
    pfd.fd = (*args->fds)[i];
    pfd.events = 0;
    pfd.revents = 0;
    pollfds.push_back(pfd);
  }

  // poll() whilst checking for cancellation at intervals
  int ready = -1; // initialized at -1 to ensure it runs at least once
  while (true) {
    int waitFor = (int) (args->timeout.diff_secs(Timestamp()) * 1000); // milliseconds
    if (waitFor <= 0) {
      if (!ready) break; // only breaks after the first time
      waitFor = 0;
    } else if (waitFor > LATER_POLL_INTERVAL) {
      waitFor = LATER_POLL_INTERVAL;
    }
    ready = LATER_POLL_FUNC(pollfds.data(), args->num_fds, waitFor);
    if (args->flag->load()) return 1;
    if (ready) break;
  }

  // store pollfd revents in args->results for use by callback
  if (ready > 0) {
    for (int i = 0; i < args->rfds; i++) {
      (*args->results)[i] = pollfds[i].revents == 0 ? 0 : pollfds[i].revents & POLLIN ? 1: R_NaInt;
    }
    for (int i = args->rfds; i < (args->rfds + args->wfds); i++) {
      (*args->results)[i] = pollfds[i].revents == 0 ? 0 : pollfds[i].revents & POLLOUT ? 1 : R_NaInt;
    }
    for (int i = args->rfds + args->wfds; i < args->num_fds; i++) {
      (*args->results)[i] = pollfds[i].revents != 0;
    }
  } else if (ready == 0) {
    std::memset(args->results->data(), 0, args->num_fds * sizeof(int));
  } else {
    for (int i = 0; i < args->num_fds; i++) {
      (*args->results)[i] = R_NaInt;
    }
  }

  execLaterNative2(later_callback, static_cast<void *>(argsptr.release()), 0, args->loop);

  return 0;

}

#else // no POLLIN: fall back to select() for R <= 4.1 on Windows

static int wait_thread(void *arg) {

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(static_cast<std::shared_ptr<ThreadArgs>*>(arg));
  std::shared_ptr<ThreadArgs> args = *argsptr;

  struct timeval tv;

  // create and zero fd_set structs
  fd_set readfds, writefds, exceptfds;
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&exceptfds);
  const int max_fd = *std::max_element(args->fds->begin(), args->fds->end());

  // select() whilst checking for cancellation at intervals
  int ready = -1; // initialized at -1 to ensure it runs at least once
  while (true) {
    // update fd_sets (already zero at this point) from args->fds
    for (int i = 0; i < args->rfds; i++) {
      FD_SET((*args->fds)[i], &readfds);
    }
    for (int i = args->rfds; i < (args->rfds + args->wfds); i++) {
      FD_SET((*args->fds)[i], &writefds);
    }
    for (int i = args->rfds + args->wfds; i < args->num_fds; i++) {
      FD_SET((*args->fds)[i], &exceptfds);
    }

    int waitFor = (int) (args->timeout.diff_secs(Timestamp()) * 1000); // milliseconds
    if (waitFor <= 0) {
      if (!ready) break; // only breaks after the first time
      waitFor = 0;
    } else if (waitFor > LATER_POLL_INTERVAL) {
      waitFor = LATER_POLL_INTERVAL;
    }
    tv.tv_sec = waitFor / 1000;
    tv.tv_usec = (waitFor % 1000) * 1000;

    ready = select(max_fd + 1, &readfds, &writefds, &exceptfds, &tv);

    if (args->flag->load()) return 1;
    if (ready) break;
  }

  // store fd_set members in args->results for use by callback
  if (ready > 0) {
    for (int i = 0; i < args->rfds; i++) {
      (*args->results)[i] = FD_ISSET((*args->fds)[i], &readfds);
    }
    for (int i = args->rfds; i < (args->rfds + args->wfds); i++) {
      (*args->results)[i] = FD_ISSET((*args->fds)[i], &writefds);
    }
    for (int i = args->rfds + args->wfds; i < args->num_fds; i++) {
      (*args->results)[i] = FD_ISSET((*args->fds)[i], &exceptfds);
    }
  } else if (ready == 0) {
    std::memset(args->results->data(), 0, args->num_fds * sizeof(int));
  } else {
    for (int i = 0; i < args->num_fds; i++) {
      (*args->results)[i] = R_NaInt;
    }
  }

  execLaterNative2(later_callback, static_cast<void *>(argsptr.release()), 0, args->loop);

  return 0;

}

#endif // POLLIN

// [[Rcpp::export]]
Rcpp::RObject execLater_fd(Rcpp::Function callback, Rcpp::IntegerVector readfds, Rcpp::IntegerVector writefds,
                           Rcpp::IntegerVector exceptfds, Rcpp::NumericVector timeoutSecs, Rcpp::IntegerVector loop_id) {

  const int rfds = static_cast<int>(readfds.size());
  const int wfds = static_cast<int>(writefds.size());
  const int efds = static_cast<int>(exceptfds.size());
  const int num_fds = rfds + wfds + efds;
  if (num_fds == 0)
    Rcpp::stop("No file descriptors supplied");

  std::shared_ptr<ThreadArgs> args = std::make_shared<ThreadArgs>();

  double timeout;
  if (timeoutSecs[0] == R_PosInf) {
    timeout = 3e10; // "1000 years ought to be enough for anybody" --Bill Gates
  } else if (timeoutSecs[0] < 0) {
    timeout = 1; // curl_multi_timeout() uses -1 to denote a default we set at 1s
  } else {
    timeout = timeoutSecs[0];
  }

  args->timeout = Timestamp(timeout);
  args->callback = std::unique_ptr<Rcpp::Function>(new Rcpp::Function(callback));
  args->flag = std::make_shared<std::atomic<bool>>();
  args->timeout = timeoutSecs[0];
  args->loop = loop_id[0];
  args->rfds = rfds;
  args->wfds = wfds;
  args->efds = efds;
  args->num_fds = num_fds;
  args->fds = std::unique_ptr<std::vector<int>>(new std::vector<int>());
  args->fds->reserve(num_fds);
  for (int i = 0; i < rfds; i++) {
    args->fds->push_back(readfds[i]);
  }
  for (int i = 0; i < wfds; i++) {
    args->fds->push_back(writefds[i]);
  }
  for (int i = 0; i < efds; i++) {
    args->fds->push_back(exceptfds[i]);
  }
  args->results = std::unique_ptr<std::vector<int>>(new std::vector<int>(num_fds));

  std::unique_ptr<std::shared_ptr<ThreadArgs>> argsptr(new std::shared_ptr<ThreadArgs>(args));

  tct_thrd_t thr;
  if (tct_thrd_create(&thr, &wait_thread, static_cast<void *>(argsptr.release())) != tct_thrd_success)
    Rcpp::stop("Thread creation failed");
  tct_thrd_detach(thr);

  Rcpp::XPtr<std::shared_ptr<std::atomic<bool>>> xptr(new std::shared_ptr<std::atomic<bool>>(args->flag), true);

  return Rcpp::RObject(xptr);

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

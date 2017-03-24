
void processx_unix_dummy() { }

#ifndef _WIN32

#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>

#include "utils.h"

/* API from R */

SEXP processx_exec(SEXP command, SEXP args, SEXP stdout, SEXP stderr,
		   SEXP detached, SEXP windows_verbatim_args,
		   SEXP windows_hide_window, SEXP private);
SEXP processx_wait(SEXP status);
SEXP processx_is_alive(SEXP status);
SEXP processx_get_exit_status(SEXP status);
SEXP processx_signal(SEXP status, SEXP signal);
SEXP processx_kill(SEXP status, SEXP grace);
SEXP processx_get_pid(SEXP status);

/* Child list and its functions */

typedef struct processx__child_list_s {
  pid_t pid;
  SEXP status;
  struct processx__child_list_s *next;
} processx__child_list_t;

static processx__child_list_t *child_list = NULL;

static void processx__child_add(pid_t pid, SEXP status);
static void processx__child_remove(pid_t pid);
static processx__child_list_t *processx__child_find(pid_t pid);

void processx__sigchld_callback(int sig, siginfo_t *info, void *ctx);
static void processx__setup_sigchld();
static void processx__remove_sigchld();
static void processx__block_sigchld();
static void processx__unblock_sigchld();

/* Other internals */

static int processx__nonblock_fcntl(int fd, int set);
static int processx__cloexec_fcntl(int fd, int set);
static void processx__child_init(processx_handle_t *handle, int pipes[3][2],
				 char *command, char **args, int error_fd,
				 const char *stdout, const char *stderr,
				 processx_options_t *options);
static void processx__collect_exit_status(SEXP status, int wstat);
static void processx__finalizer(SEXP status);

SEXP processx__make_handle(SEXP private);

void R_unload_processx(DllInfo *dll) {
  processx__remove_sigchld();
}

static int processx__nonblock_fcntl(int fd, int set) {
  int flags;
  int r;

  do { r = fcntl(fd, F_GETFL); } while (r == -1 && errno == EINTR);
  if (r == -1) { return -errno; }

  /* Bail out now if already set/clear. */
  if (!!(r & O_NONBLOCK) == !!set) { return 0; }

  if (set) { flags = r | O_NONBLOCK; } else { flags = r & ~O_NONBLOCK; }

  do { r = fcntl(fd, F_SETFL, flags); } while (r == -1 && errno == EINTR);
  if (r) { return -errno; }

  return 0;
}

static int processx__cloexec_fcntl(int fd, int set) {
  int flags;
  int r;

  do { r = fcntl(fd, F_GETFD); } while (r == -1 && errno == EINTR);
  if (r == -1) { return -errno; }

  /* Bail out now if already set/clear. */
  if (!!(r & FD_CLOEXEC) == !!set) { return 0; }

  if (set) { flags = r | FD_CLOEXEC; } else { flags = r & ~FD_CLOEXEC; }

  do { r = fcntl(fd, F_SETFD, flags); } while (r == -1 && errno == EINTR);
  if (r) { return -errno; }

  return 0;
}

void processx__write_int(int fd, int err) {
  int dummy = write(fd, &err, sizeof(int));
  (void) dummy;
}

static void processx__child_init(processx_handle_t* handle, int pipes[3][2],
				 char *command, char **args, int error_fd,
				 const char *stdout, const char *stderr,
				 processx_options_t *options) {

  int fd0, fd1, fd2;

  if (options->detached) setsid();

  /* stdin is coming from /dev/null */

  fd0 = open("/dev/null", O_RDONLY);
  if (fd0 == -1) { processx__write_int(error_fd, - errno); raise(SIGKILL); }

  if (fd0 != 0) fd0 = dup2(fd0, 0);
  if (fd0 == -1) { processx__write_int(error_fd, - errno); raise(SIGKILL); }

  /* stdout is going into file or a pipe */

  if (!stdout) {
    fd1 = open("/dev/null", O_RDWR);
  } else if (!strcmp(stdout, "|")) {
    fd1 = pipes[1][1];
    close(pipes[1][0]);
  } else {
    fd1 = open(stdout, O_CREAT | O_TRUNC| O_RDWR, 0644);
  }
  if (fd1 == -1) { processx__write_int(error_fd, - errno); raise(SIGKILL); }

  if (fd1 != 1) fd1 = dup2(fd1, 1);
  if (fd1 == -1) { processx__write_int(error_fd, - errno); raise(SIGKILL); }

  /* stderr, to file or a pipe */

  if (!stderr) {
    fd2 = open("/dev/null", O_RDWR);
  } else if (!strcmp(stderr, "|")) {
    fd2 = pipes[2][1];
    close(pipes[2][0]);
  } else {
    fd2 = open(stderr, O_CREAT | O_TRUNC| O_RDWR, 0644);
  }
  if (fd2 == -1) { processx__write_int(error_fd, - errno); raise(SIGKILL); }

  if (fd2 != 2) fd2 = dup2(fd2, 2);
  if (fd2 == -1) { processx__write_int(error_fd, - errno); raise(SIGKILL); }

  processx__nonblock_fcntl(fd0, 0);
  processx__nonblock_fcntl(fd1, 0);
  processx__nonblock_fcntl(fd2, 0);

  execvp(command, args);
  processx__write_int(error_fd, - errno);
  raise(SIGKILL);
}

static void processx__finalizer(SEXP status) {
  processx_handle_t *handle = (processx_handle_t*) R_ExternalPtrAddr(status);
  pid_t pid;
  int wp, wstat;
  SEXP private;

  processx__block_sigchld();

  /* Already freed? */
  if (!handle) goto cleanup;

  /* Do a non-blocking waitpid() to see if it is running */
  pid = handle->pid;
  do {
    wp = waitpid(pid, &wstat, WNOHANG);
  } while (wp == -1 && errno == EINTR);

  /* Maybe just waited on it? Then collect status */
  if (wp == pid) processx__collect_exit_status(status, wstat);

  /* If it is running, we need to kill it, and wait for the exit status */
  if (wp == 0) {
    kill(pid, SIGKILL);
    do {
      wp = waitpid(pid, &wstat, 0);
    } while (wp == -1 && errno == EINTR);
    processx__collect_exit_status(status, wstat);
  }

  /* It is dead now, copy over pid and exit status */
  private = R_ExternalPtrTag(status);
  defineVar(install("exited"), ScalarLogical(1), private);
  defineVar(install("pid"), ScalarInteger(pid), private);
  defineVar(install("exitcode"), ScalarInteger(handle->exitcode), private);

  /* Deallocate memory */
  R_ClearExternalPtr(status);
  processx__handle_destroy(handle);

 cleanup:
  processx__unblock_sigchld();
}

SEXP processx__make_handle(SEXP private) {
  processx_handle_t * handle;
  SEXP result;

  handle = (processx_handle_t*) malloc(sizeof(processx_handle_t));
  if (!handle) { error("Out of memory"); }
  memset(handle, 0, sizeof(processx_handle_t));

  result = PROTECT(R_MakeExternalPtr(handle, private, R_NilValue));
  R_RegisterCFinalizerEx(result, processx__finalizer, 1);

  UNPROTECT(1);
  return result;
}

static void processx__child_add(pid_t pid, SEXP status) {
  processx__child_list_t *child = malloc(sizeof(processx__child_list_t));
  child->pid = pid;
  child->status = status;
  child->next = child_list;
  child_list = child;
}

static void processx__child_remove(pid_t pid) {
  processx__child_list_t *ptr = child_list, *prev = 0;
  while (ptr) {
    if (ptr->pid == pid) {
      if (prev) {
	prev->next = ptr->next;
	free(ptr);
      }
      return;
    }
    prev = ptr;
    ptr = ptr->next;
  }
}

static processx__child_list_t *processx__child_find(pid_t pid) {
  processx__child_list_t *ptr = child_list;
  while (ptr) {
    if (ptr->pid == pid) return ptr;
    ptr = ptr->next;
  }
  return 0;
}

void processx__sigchld_callback(int sig, siginfo_t *info, void *ctx) {
  if (sig != SIGCHLD) return;
  pid_t pid = info->si_pid;
  processx__child_list_t *child = processx__child_find(pid);

  if (child) {
    /* We deliberately do not call the finalizer here, because that
       moves the exit code and pid to R, and we might have just checked
       that these are not in R, before calling C. So finalizing here
       would be a race condition.

       OTOH, we need to check if the handle is null, because a finalizer
       might actually run before the SIGCHLD handler. Or the finalizer
       might even trigger the SIGCHLD handler...
    */
    int wp, wstat;
    processx_handle_t *handle = R_ExternalPtrAddr(child->status);

    /* This might not be necessary, if the handle was finalized,
       but it does not hurt... */
    do {
      wp = waitpid(pid, &wstat, 0);
    } while (wp == -1 && errno == EINTR);

    /* If handle is NULL, then the exit status was collected already */
    if (handle) processx__collect_exit_status(child->status, wstat);

    processx__child_remove(pid);

    /* If no more children, then we do not need a SIGCHLD handler */
    if (!child_list) processx__remove_sigchld();
  }
}

/* TODO: use oldact */

static void processx__setup_sigchld() {
  struct sigaction action;
  action.sa_sigaction = processx__sigchld_callback;
  action.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &action, /* oldact= */ NULL);
}

static void processx__remove_sigchld() {
  struct sigaction action;
  action.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &action, /* oldact= */ NULL);
}

static void processx__block_sigchld() {
  sigset_t blockMask;
  sigemptyset(&blockMask);
  sigaddset(&blockMask, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &blockMask, NULL) == -1) {
    error("processx error setting up signal handlers");
  }
}

static void processx__unblock_sigchld() {
  sigset_t unblockMask;
  sigemptyset(&unblockMask);
  sigaddset(&unblockMask, SIGCHLD);
  if (sigprocmask(SIG_UNBLOCK, &unblockMask, NULL) == -1) {
    error("processx error setting up signal handlers");
  }
}

void processx__make_socketpair(int pipe[2]) {
#if defined(__linux__)
  static int no_cloexec;
  if (no_cloexec)  goto skip;

  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pipe) == 0)
    return;

  /* Retry on EINVAL, it means SOCK_CLOEXEC is not supported.
   * Anything else is a genuine error.
   */
  if (errno != EINVAL) {
    error("processx socketpair: %s", strerror(errno));
  }

  no_cloexec = 1;

skip:
#endif

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipe)) {
    error("processx socketpair: %s", strerror(errno));
  }

  processx__cloexec_fcntl(pipe[0], 1);
  processx__cloexec_fcntl(pipe[1], 1);
}

SEXP processx_exec(SEXP command, SEXP args, SEXP stdout, SEXP stderr,
		   SEXP detached, SEXP windows_verbatim_args,
		   SEXP windows_hide_window, SEXP private) {

  char *ccommand = processx__tmp_string(command, 0);
  char **cargs = processx__tmp_character(args);
  const char *cstdout = isNull(stdout) ? 0 : CHAR(STRING_ELT(stdout, 0));
  const char *cstderr = isNull(stderr) ? 0 : CHAR(STRING_ELT(stderr, 0));
  processx_options_t options = { 0 };

  pid_t pid;
  int err, exec_errorno = 0, status;
  ssize_t r;
  int signal_pipe[2] = { -1, -1 };
  int pipes[3][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };

  processx_handle_t *handle = NULL;
  SEXP result;

  options.detached = LOGICAL(detached)[0];

  if (pipe(signal_pipe)) { goto cleanup; }
  processx__cloexec_fcntl(signal_pipe[0], 1);
  processx__cloexec_fcntl(signal_pipe[1], 1);

  processx__setup_sigchld();

  result = PROTECT(processx__make_handle(private));
  handle = R_ExternalPtrAddr(result);

  /* Create pipes, if requested. TODO: stdin */
  if (cstdout && !strcmp(cstdout, "|")) processx__make_socketpair(pipes[1]);
  if (cstderr && !strcmp(cstderr, "|")) processx__make_socketpair(pipes[2]);

  processx__block_sigchld();

  pid = fork();

  if (pid == -1) {		/* ERROR */
    err = -errno;
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    processx__unblock_sigchld();
    goto cleanup;
  }

  /* CHILD */
  if (pid == 0) {
    processx__child_init(handle, pipes, ccommand, cargs, signal_pipe[1],
			 cstdout, cstderr, &options);
    goto cleanup;
  }

  /* We need to know the processx children */
  processx__child_add(pid, result);

  /* SIGCHLD can arrive now */
  processx__unblock_sigchld();

  close(signal_pipe[1]);

  do {
    r = read(signal_pipe[0], &exec_errorno, sizeof(exec_errorno));
  } while (r == -1 && errno == EINTR);

  if (r == 0) {
    ; /* okay, EOF */
  } else if (r == sizeof(exec_errorno)) {
    do {
      err = waitpid(pid, &status, 0); /* okay, read errorno */
    } while (err == -1 && errno == EINTR);

  } else if (r == -1 && errno == EPIPE) {
    do {
      err = waitpid(pid, &status, 0); /* okay, got EPIPE */
    } while (err == -1 && errno == EINTR);

  } else {
    goto cleanup;
  }

  close(signal_pipe[0]);

  /* Set fds for standard I/O */
  /* TODO: implement stdin */
  handle->fd0 = handle->fd1 = handle->fd2 = -1;
  if (pipes[1][0] >= 0) {
    handle->fd1 = pipes[1][0];
    processx__nonblock_fcntl(handle->fd1, 1);
  }
  if (pipes[2][0] >= 0) {
    handle->fd2 = pipes[2][0];
    processx__nonblock_fcntl(handle->fd2, 1);
  }

  /* Closed unused ends of pipes */
  if (pipes[1][1] >= 0) close(pipes[1][1]);
  if (pipes[2][1] >= 0) close(pipes[2][1]);

  if (exec_errorno == 0) {
    handle->pid = pid;
    UNPROTECT(1);		/* result */
    return result;
  }

 cleanup:
  error("processx error");
}

/* Process status (and related functions).

   The main complication here, is that checking the status of the process
   might mean that we need to collect its exit status.

   * `process_wait`:
     1. If we already have its exit status, return immediately.
     2. Otherwise, do a blocking `waitpid()`.
     3. When it's done, collect the exit status.

   * `process_is_alive`:
     1. If we already have its exit status, then return `FALSE`.
     2. Otherwise, do a non-blocking `waitpid()`.
     3. If the `waitpid()` says that it is running, then return `TRUE`.
     4. Otherwise collect its exit status, and return `FALSE`.

   * `process_get_exit_status`:
     1. If we already have the exit status, then return that.
     2. Otherwise do a non-blocking `waitpid()`.
     3. If the process just finished, then collect the exit status, and
        also return it.
     4. Otherwise return `NULL`, the process is still running.

   * `process_signal`:
     1. If we already have its exit status, return with `FALSE`.
     2. Otherwise just try to deliver the signal. If successful, return
        `TRUE`, otherwise return `FALSE`.

     We might as well call `waitpid()` as well, but `process_signal` is
     able to deliver arbitrary signals, so the process might not have
     finished.

   * `process_kill`:
     1. Check if we have the exit status. If yes, then the process
        has already finished. and we return `FALSE`. We don't error,
        because then there would be no way to deliver a signal.
        (Simply doing `if (p$is_alive()) p$kill()` does not work, because
        it is a race condition.
     2. If there is no exit status, the process might be running (or might
        be a zombie).
     3. We call a non-blocking `waitpid()` on the process and potentially
        collect the exit status. If the process has exited, then we return
        TRUE. This step is to avoid the potential grace period, if the
        process is in a zombie state.
     4. If the process is still running, we call `kill(SIGKILL)`.
     5. We do a blocking `waitpid()` to collect the exit status.
     6. If the process was indeed killed by us, we return `TRUE`.
     7. Otherwise we return `FALSE`.

    The return value of `process_kill()` is `TRUE` if the process was
    indeed killed by the signal. It is `FALSE` otherwise, i.e. if the
    process finished.

    We currently ignore the grace argument, as there is no way to
    implement it on Unix. It will be implemented later using a SIGCHLD
    handler.

   * Finalizers (`processx__finalizer`):

     Finalizers are called on the handle only, so we do not know if the
     process has already finished or not.

     1. Call a non-blocking `waitpid()` to see if it is still running.
     2. If just finished, then collect exit status (=free memory).
     3. If it has finished before, then still try to free memory, just in
        case the exit status was read out by another package.
     4. If it is running, then kill it with SIGKILL, then call a blocking
        `waitpid()` to clean up the zombie process. Then free all memory.

     The finalizer is implemented in C, because we might need to use it
     from the process startup code (which is C).
*/

void processx__collect_exit_status(SEXP status, int wstat) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);

  /* This must be called from a function that blocks SIGCHLD.
     So we are not blocking it here. */

  if (!handle) {
    error("Invalid handle, already finalized");
  }

  if (handle->collected) { return; }

  /* We assume that errors were handled before */
  if (WIFEXITED(wstat)) {
    handle->exitcode = WEXITSTATUS(wstat);
  } else {
    handle->exitcode = - WTERMSIG(wstat);
  }

  handle->collected = 1;
}

SEXP processx_wait(SEXP status) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);
  pid_t pid;
  int wstat, wp;

  processx__block_sigchld();

  if (!handle) {
    processx__unblock_sigchld();
    error("Internal processx error, handle already removed");
  }

  /* If we already have the status, then return now. */
  if (handle->collected) goto cleanup;

  /* Otherwise do a blocking waitpid */
  pid = handle->pid;
  do {
    wp = waitpid(pid, &wstat, 0);
  } while (wp == -1 && errno == EINTR);

  /* Error? */
  if (wp == -1) {
    processx__unblock_sigchld();
    error("processx_wait: %s", strerror(errno));
  }

  /* Collect exit status */
  processx__collect_exit_status(status, wstat);

 cleanup:
  processx__unblock_sigchld();
  return R_NilValue;
}

SEXP processx_is_alive(SEXP status) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);
  pid_t pid;
  int wstat, wp;
  int ret = 0;

  processx__block_sigchld();

  if (!handle) {
    processx__unblock_sigchld();
    error("Internal processx error, handle already removed");
  }

  if (handle->collected) goto cleanup;

  /* Otherwise a non-blocking waitpid to collect zombies */
  pid = handle->pid;
  do {
    wp = waitpid(pid, &wstat, WNOHANG);
  } while (wp == -1 && errno == EINTR);

  /* Some other error? */
  if (wp == -1) {
    processx__unblock_sigchld();
    error("processx_is_alive: %s", strerror(errno));
  }

  /* If running, return TRUE, otherwise collect exit status, return FALSE */
  if (wp == 0) {
    ret = 1;
  } else {
    processx__collect_exit_status(status, wstat);
  }

 cleanup:
  processx__unblock_sigchld();
  return ScalarLogical(ret);
}

SEXP processx_get_exit_status(SEXP status) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);
  pid_t pid;
  int wstat, wp;
  SEXP result;

  processx__block_sigchld();

  if (!handle) {
    processx__unblock_sigchld();
    error("Internal processx error, handle already removed");
  }

  /* If we already have the status, then just return */
  if (handle->collected) {
    result = PROTECT(ScalarInteger(handle->exitcode));
    goto cleanup;
  }

  /* Otherwise do a non-blocking waitpid to collect zombies */
  pid = handle->pid;
  do {
    wp = waitpid(pid, &wstat, WNOHANG);
  } while (wp == -1 && errno == EINTR);

  /* Some other error? */
  if (wp == -1) {
    processx__unblock_sigchld();
    error("processx_get_exit_status: %s", strerror(errno));
  }

  /* If running, do nothing otherwise collect */
  if (wp == 0) {
    result = PROTECT(R_NilValue);
  } else {
    processx__collect_exit_status(status, wstat);
    result = PROTECT(ScalarInteger(handle->exitcode));
  }

 cleanup:
  processx__unblock_sigchld();
  UNPROTECT(1);
  return result;
}

SEXP processx_signal(SEXP status, SEXP signal) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);
  pid_t pid;
  int wstat, wp, ret, result;

  processx__block_sigchld();

  if (!handle) {
    processx__unblock_sigchld();
    error("Internal processx error, handle already removed");
  }

  /* If we already have the status, then return `FALSE` */
  if (handle->collected) {
    result = 0;
    goto cleanup;
  }

  /* Otherwise try to send signal */
  pid = handle->pid;
  ret = kill(pid, INTEGER(signal)[0]);

  if (ret == 0) {
    result = 1;
  } else if (ret == -1 && errno == ESRCH) {
    result = 0;
  } else {
    processx__unblock_sigchld();
    error("processx_signal: %s", strerror(errno));
    return R_NilValue;
  }

  /* Dead now, collect status */
  do {
    wp = waitpid(pid, &wstat, WNOHANG);
  } while (wp == -1 && errno == EINTR);

  if (wp == -1) {
    processx__unblock_sigchld();
    error("processx_get_exit_status: %s", strerror(errno));
  }

 cleanup:
  processx__unblock_sigchld();
  return ScalarLogical(result);
}

SEXP processx_kill(SEXP status, SEXP grace) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);
  pid_t pid;
  int wstat, wp, result = 0;

  processx__block_sigchld();

  if (!handle) {
    processx__unblock_sigchld();
    error("Internal processx error, handle already removed");
  }

  /* Check if we have an exit status, it yes, just return (FALSE) */
  if (handle->collected) { goto cleanup; }

  /* Do a non-blocking waitpid to collect zombies */
  pid = handle->pid;
  do {
    wp = waitpid(pid, &wstat, WNOHANG);
  } while (wp == -1 && errno == EINTR);

  /* Some other error? */
  if (wp == -1) {
    processx__unblock_sigchld();
    error("processx_kill: %s", strerror(errno));
  }

  /* If the process is not running, return (FALSE) */
  if (wp != 0) { goto cleanup; }

  /* It is still running, so a SIGKILL */
  int ret = kill(pid, SIGKILL);
  if (ret == -1 && errno == ESRCH) { goto cleanup; }
  if (ret == -1) {
    processx__unblock_sigchld();
    error("process_kill: %s", strerror(errno));
  }

  /* Do a waitpid to collect the status and reap the zombie */
  do {
    wp = waitpid(pid, &wstat, 0);
  } while (wp == -1 && errno == EINTR);

  /* Collect exit status, and check if it was killed by a SIGKILL
     If yes, this was most probably us (although we cannot be sure in
     general... */
  processx__collect_exit_status(status, wstat);
  result = handle->exitcode == - SIGKILL;

 cleanup:
  processx__unblock_sigchld();
  return ScalarLogical(result);
}

SEXP processx_get_pid(SEXP status) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);

  if (!handle) { error("Internal processx error, handle already removed"); }

  return ScalarInteger(handle->pid);
}

SEXP processx_read_output_lines(SEXP status) {
  /* TODO */
}

SEXP processx_read_output(SEXP status) {
  processx_handle_t *handle = R_ExternalPtrAddr(status);
  ssize_t num;
  char buffer[4096];
  SEXP result;

  if (!handle) { error("Internal processx error, handle already removed"); }
  if (handle->fd1 < 0) { error("No stdout pipe exists for this process"); }

  num = read(handle->fd1, buffer, sizeof(buffer));

  if (num < 0 && errno == EAGAIN) {
    /* Not closed, but no data currently */
    result = PROTECT(mkString(""));

  } else if (num < 0) {
    error("processx read error: %s", strerror(errno));

  } else if (num == 0) {
    /* Closed, EOF */
    result = PROTECT(mkString(""));
    setAttrib(result, install("eof"), ScalarLogical(1));

  } else {
    /* Data */
    result = PROTECT(allocVector(STRSXP, 1));
    SET_STRING_ELT(result, 0, mkCharLen(buffer, num));
  }

  UNPROTECT(1);
  return result;
}

SEXP processx_read_error_lines(SEXP status) {
  /* TODO */
}

SEXP processx_read_error(SEXP status) {
  /* TODO */
}

SEXP processx_can_read_output(SEXP status) {
  /* TODO */
}

SEXP processx_can_read_error(SEXP status) {
  /* TODO */
}

SEXP processx_is_eof_output(SEXP status) {
  /* TODO */
}

SEXP processx_is_eof_error(SEXP status) {
  /* TODO */
}

#endif

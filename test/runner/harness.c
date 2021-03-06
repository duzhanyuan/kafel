/*
   Kafel - test harness
   -----------------------------------------

   Copyright 2017 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include "harness.h"

#include <errno.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <kafel.h>

#include "runner.h"

#define TEST_PASSED() \
  do {                \
    return 0;         \
  } while (0)

#define TEST_FAIL(fmt, ...)                     \
  do {                                          \
    test_fail_with_message(fmt, ##__VA_ARGS__); \
    return -1;                                  \
  } while (0)

static struct sock_fprog test_policy_prog = {0, NULL};
static bool test_policy_compilation_flag;

int test_policy(const char* source) {
  free(test_policy_prog.filter);
  test_policy_prog.len = 0;
  kafel_ctxt_t ctxt = kafel_ctxt_create();
  kafel_set_input_string(ctxt, source);
  int rv = kafel_compile(ctxt, &test_policy_prog);
  if (rv != 0) {
    test_policy_compilation_flag = false;
    TEST_FAIL("Compilation failure:\n\t%s", kafel_error_msg(ctxt));
  }
  test_policy_compilation_flag = true;
  TEST_PASSED();
}

static void sys_exit(int rv) { syscall(__NR_exit, rv); }

static void install_seccomp_prog(struct sock_fprog* prog) {
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
    sys_exit(-1);
  }
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, prog, 0, 0)) {
    sys_exit(-1);
  }
}

static void kill_and_wait(pid_t pid) {
  if (kill(pid, SIGKILL) == 0) {
    waitpid(pid, NULL, 0);
  } else {
    waitpid(pid, NULL, WNOHANG);
  }
}

int test_policy_enforcment(test_func_t test_func, void* data,
                           bool should_kill) {
  // Skip tests when compilation failed
  if (!test_policy_compilation_flag) {
    TEST_PASSED();
  }

  sigset_t sigchld_set;
  sigset_t orig_set;
  sigemptyset(&sigchld_set);
  sigaddset(&sigchld_set, SIGCHLD);
  sigprocmask(SIG_BLOCK, &sigchld_set, &orig_set);
  pid_t pid = fork();
  if (pid == -1) {
    sigprocmask(SIG_SETMASK, &orig_set, NULL);
    TEST_FAIL("could not fork");
  } else if (pid == 0) {
    install_seccomp_prog(&test_policy_prog);
    sys_exit(test_func(data));
  }
  int sigchld_fd = signalfd(-1, &sigchld_set, 0);
  if (sigchld_fd < 0) {
    kill_and_wait(pid);
    sigprocmask(SIG_SETMASK, &orig_set, NULL);
    TEST_FAIL("signalfd failed");
  }
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(sigchld_fd, &rfds);
  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  int rv = select(sigchld_fd + 1, &rfds, NULL, NULL, &timeout);
  while (rv < 0) {
    if (errno != EINTR) {
      close(sigchld_fd);
      kill_and_wait(pid);
      sigprocmask(SIG_SETMASK, &orig_set, NULL);
      TEST_FAIL("select failed");
    }
    rv = select(sigchld_fd + 1, &rfds, NULL, NULL, &timeout);
  }
  close(sigchld_fd);
  if (rv == 0) {
    kill_and_wait(pid);
    sigprocmask(SIG_SETMASK, &orig_set, NULL);
    TEST_FAIL("timed out");
  }
  sigprocmask(SIG_SETMASK, &orig_set, NULL);
  siginfo_t si;
  si.si_pid = 0;
  rv = waitid(P_PID, pid, &si, WEXITED | WNOHANG);
  if (rv != 0 || si.si_pid != pid) {
    kill_and_wait(pid);
    TEST_FAIL("waitid failed %d %d %d %d", rv, errno, si.si_pid, pid);
  }
  if (si.si_code == CLD_EXITED && si.si_status != 0) {
    if (should_kill) {
      TEST_FAIL("should be killed by seccomp; non-zero (%d) exit code instead",
                si.si_status);
    } else {
      TEST_FAIL("non-zero (%d) exit code", si.si_status);
    }
  }
  if (should_kill && (si.si_code != CLD_KILLED || si.si_status != SIGSYS)) {
    TEST_FAIL("should be killed by seccomp");
  }
  if (si.si_code == CLD_KILLED) {
    if (si.si_status == SIGSYS) {
      if (!should_kill) {
        TEST_FAIL("should not be killed by seccomp");
      }
    } else {
      TEST_FAIL("killed by signal %d", si.si_status);
    }
  } else if (si.si_code != CLD_EXITED) {
    TEST_FAIL("not exited normally");
  }
  TEST_PASSED();
}

static int syscall_caller_helper(void* data) {
  int syscall_no = 0;
  for (const syscall_exec_spec_t* syscall_spec =
           (const syscall_exec_spec_t*)data;
       !syscall_spec->is_last; ++syscall_spec) {
    ++syscall_no;
    long nr = syscall_spec->syscall.nr;
    const long* arg = syscall_spec->syscall.args;
    long expected = syscall_spec->result.rv;
    long expected_errno = syscall_spec->result.expected_errno;
    long ret = syscall(nr, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]);
    if (ret != expected || errno != expected_errno) {
      return syscall_no;
    }
  }
  return 0;
}

int test_policy_enforcment_syscalls(syscall_exec_spec_t syscall_specs[],
                                    bool should_kill) {
  return test_policy_enforcment(syscall_caller_helper, syscall_specs,
                                should_kill);
}

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>

#define MAXARGS 128
#define MAXLINE 8192
#define MAXJOBS 100

extern char *environ[];

void eval(char *cmdline);
int parseline(char *buf, char **argv);
int builtin_command(char **argv);
void run_fg(pid_t);
void run_bg(pid_t pid, char *);
int parse_int(char *);
int parse_pid(char *);

static char sigbuf[100];

typedef enum {
  UNINIT,
  RUNNING,
  STOPPED,
  TERMINATED,
} Status;

// TODO: heap-allocate the whole command for this job.
// or allocate 12-byte string and trim the command to fit.
// 12 instead of 8 because there will be 4-byte padding when
// we use 8, so why not use the other 4 bytes.
typedef struct {
  pid_t pid;
  int jid;
  Status st;
} job;

static int next_jid = 0;
static job jobs[MAXJOBS];

// Returns -1 when there are no empty slots to add the new job.
// addjob is used in two scenarios:
// 1. When adding a new job.
// 2. When resuming a suspended job and running it in the background.
// TODO: remove updating the status from this function and use setjobstatus
// instead.
int addjob(pid_t pid, Status st) {
  job *free_job;
  for (int i = 0; i < MAXJOBS; i++) {
    job *j = &jobs[i];

    // An entry already exists for this pid;
    if (j->pid == pid) {
      j->st = st;
      return j->jid;
    }

    if (j->st == UNINIT || j->st == TERMINATED) {
      free_job = j;
      continue;
    }
  }

  // If managed to find an empty job slot, use it.
  if (free_job != NULL) {
    int jid = ++next_jid;
    free_job->jid = jid;
    free_job->pid = pid;
    free_job->st = st;
    return jid;
  }

  return -1;
}

// Returns jid when there is a match for the given pid, or -1 otherwise.
int setjobstat(pid_t pid, Status st) {
  for (int i = 0; i < MAXJOBS; i++) {
    job *j = &jobs[i];

    // An entry already exists for this pid;
    if (j->pid == pid) {
      j->st = st;
      return j->jid;
    }
  }
  return -1;
}

job *findjob(pid_t pid) {
  for (int i = 0; i < MAXJOBS; i++) {
    job *j = &jobs[i];

    // An entry already exists for this pid;
    // TODO: should it ignore terminated jobs?
    if (j->pid == pid) {
      return j;
    }
  }
  return NULL;
}

char *jstatus_str(Status st) {
  switch (st) {
  case UNINIT:
    return "UNINIT";
  case RUNNING:
    return "RUNNING";
  case STOPPED:
    return "STOPPED";
  case TERMINATED:
    return "TERMINATED";
  }

  return "";
}

// Used to forward Ctrl+C/Ctrl+Z to the foreground process group.
// When a process is running in foreground, it should be set to
// pgid of that process. When a command is run with &, this should
// be set to 0 and there will be no signal forwarding.
static int fg_pid = 0;

void unix_error(char *msg) {
  fprintf(stderr, "%s: %s\n", msg, strerror(errno));
  exit(0);
}

// handles SIGINT and SIGTSTP.
void forward_signal(int sig) {
  // No need to save and restore errno as we don't expected any errors here.
  sigset_t mask_all, prev_all;
  if (sigfillset(&mask_all) < 0)
    unix_error("sigfillset error");

  if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0)
    unix_error("sigprocmask block error");

  // Only forward if there is a foreground process, otherwise
  // ignore the signal.
  if (fg_pid) {
    printf("[ForwardSignalHandler] forwarding sig %d to %d\n", sig, fg_pid);
    /* It's tempting to try to handle the error and not fail in case
     * of ESRCH, but we're making sure that we will only call kill for
     * valid pids by checking fg_pid, and we want such errors to be deteced.
     * Addtionally all signals are blocked when this function starts, so it
     * eliminates the possibility of receiving SIGINT while handling SIGTSTP,
     * thus we won't end up sending the same signal twice to a process.
     * Due to implicit signal blocking, receiving the same signal won't
     * interrupt handling another, even without an explicit sigprocmask. */
    if (kill(fg_pid, sig) < 0) {
      unix_error("Forward SIGINT|SIGTSTP error");
    }
  }

  if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0)
    unix_error("sigprocmask set mask error");
}

// Reaps children and updates the job status to be one of the following:
// TERMINATED: when child exists normally or terminated by a signal.
// STOPPED: when child is suspended with SIGTSTP.
// RUNNING: when child resumes execution.
void reap_child(int sig) {
  sigset_t mask_all, prev_all;
  if (sigfillset(&mask_all) < 0)
    unix_error("sigfillset error");

  if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0)
    unix_error("sigprocmask block error");

  int status;
  pid_t pid;

  // We want to be informed if children were terminated, stopped or continued
  // so that we can keep update their status accordingly.
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
    Status st;
    if (WIFEXITED(status) || WIFSIGNALED(status))
      st = TERMINATED;
    else if (WIFSTOPPED(status))
      st = STOPPED;
    else // WIFCONTINUED
      st = RUNNING;
    printf("[ReapChildHandler] setjobstat %d to %s\n", pid, jstatus_str(st));
    setjobstat(pid, st);
  }

  assert(pid == 0 || errno == ECHILD);

  if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0)
    unix_error("sigprocmask set mask error");
}

pid_t Fork() {
  pid_t pid;

  if ((pid = fork()) < 0)
    unix_error("Fork error");
  return pid;
}

int main() {
  sigset_t mask_one, prev_one;
  if (sigemptyset(&mask_one) < 0)
    unix_error("sigempty error");
  if (sigaddset(&mask_one, SIGCHLD) < 0)
    unix_error("sigaddset error");

  if (signal(SIGINT, forward_signal) == SIG_ERR) {
    unix_error("Install SIGINT handler error");
  }
  if (signal(SIGTSTP, forward_signal) == SIG_ERR) {
    unix_error("Install SIGTSTP handler error");
  }
  if (signal(SIGCHLD, reap_child) == SIG_ERR) {
    unix_error("Install SIGCHLD handler error");
  }

  char cmdline[MAXLINE];
  while (1) {
    printf("> ");
    fgets(cmdline, MAXLINE, stdin);
    // Ctrl+D sends EOF signal.
    if (feof(stdin))
      exit(0);

    // Block SIGCHLD before eval. In the next loop iter when we get to fgets
    // SIGCHLD handler will get a chance to run. We don't have to block all
    // signals because we still want to receive SIGINT and SIGTSTP and forward
    // them to the foreground process.
    if (sigprocmask(SIG_BLOCK, &mask_one, &prev_one) < 0)
      unix_error("sigprocmask block error");

    eval(cmdline);

    if (sigprocmask(SIG_SETMASK, &prev_one, NULL) < 0)
      unix_error("sigprocmask set mask error");
  }
}

void eval(char *cmdline) {
  char *argv[MAXARGS];
  char buf[MAXLINE];
  int bg;
  pid_t pid;

  strcpy(buf, cmdline);
  bg = parseline(buf, argv);

  if (argv[0] == NULL) /* Ignore empty commands. */
    return;
  if (!strcmp(argv[0], "&")) /* Ignore singleton '&'. */
    return;

  if (!strcmp(argv[0], "fg")) {
    if (argv[1] == NULL) {
      return;
    }

    pid_t stp_pid;
    char *id_part = argv[1];

    if ((stp_pid = parse_pid(id_part)) > 0) {
      // stp_pid is the pid of a stopped background process which now
      // needs to resume and run in foreground.
      if (kill(stp_pid, SIGCONT) < 0) {
        unix_error("Forward SIGCONT error");
      }
      run_fg(stp_pid);
      return;
    }

    printf("%s: No such process\n", argv[1]);
    return;
  }

  if (!strcmp(argv[0], "bg")) {
    if (argv[1] == NULL) {
      return;
    }

    pid_t stp_pid;
    char *id_part = argv[1];

    if ((stp_pid = parse_pid(id_part)) > 0) {
      // stp_pid is the pid of a stopped background process which now
      // needs to resume and run in background.
      //
      // If the process already terminated, we should have received SIGCHLD for
      // it before the current run of eval which will change its status, thus
      // parse_pid will return -1.
      if (kill(stp_pid, SIGCONT) < 0) {
        unix_error("Forward SIGCONT error");
      }
      run_bg(stp_pid, "");
      return;
    }

    printf("%s: No such process\n", argv[1]);
    return;
  }

  if (!builtin_command(argv)) {
    /* START - CHILD PROCESS */
    if ((pid = Fork()) == 0) {
      // Set process group ID of this job to the pid of child.
      if (setpgid(0, 0) < 0) {
        unix_error("setpgid error");
      }
      if (execve(argv[0], argv, environ) < 0) {
        printf("%s: Command not found.\n", argv[0]);
        exit(0);
      }
    }
    /* END - CHILD PROCESS */

    if (!bg) {
      run_fg(pid);
    } else {
      run_bg(pid, cmdline);
    }
    return;
  }

  return;
}

int parseline(char *buf, char **argv) {
  char *delim;
  int argc;
  int bg;

  buf[strlen(buf) - 1] = ' ';
  // Trim leading spaces.
  while (*buf && (*buf == ' '))
    buf++;

  argc = 0;
  while ((delim = strchr(buf, ' '))) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' '))
      buf++;
  }
  argv[argc] = NULL;

  if (argc == 0)
    return 1;

  if ((bg = (*argv[argc - 1] == '&')) != 0)
    argv[--argc] = NULL;

  return bg;
}

// Return value is -1, 0 or a positive integers.
// 1: it was a built-in command and handled already.
// 0: it was not build-in command.
int builtin_command(char **argv) {
  // strcmp return 0 if strings are equal.
  if (!strcmp(argv[0], "quit"))
    exit(0);

  if (!strcmp(argv[0], "jobs")) {
    for (int i = 0; i < MAXJOBS; i++) {
      job j = jobs[i];
      if (j.st != TERMINATED && j.st != UNINIT) {
        printf("[%d] %d %s\n", j.jid, j.pid, jstatus_str(j.st));
      }
    }
    return 1;
  }

  return 0;
}

// Assuming s (argv) has at least two elements, it performs no bound checking.
// Returns values are -1 or a value greater than 0.
// >0: pid of a process which should run in foreground.
// -1: if there were any errors parsing pid pr if the job is uninit or has
//     has already finished.
pid_t parse_pid(char *s) {
  if (*s == '%') {
    // parse jid after %, and find a correspoding pid for it.
    int jid = parse_int(++s);

    // Avoid matching against the default value (0) in jobs array.
    // And return early in case of errors (-1) in parse_int.
    if (jid <= 0)
      return -1;

    // check status is not terminated or uninit.
    for (int i = 0; i < MAXJOBS; i++) {
      job *j = &jobs[i];
      if (j->st == TERMINATED && j->st == UNINIT)
        return -1;
      if (j->jid == jid) {
        return jobs[i].pid;
      }
    }

    // Not being able to find a match for a jid is considered error.
    return -1;
  }

  int pid = parse_int(s);
  for (int i = 0; i < MAXJOBS; i++) {
    job *j = &jobs[i];
    if (j->st == TERMINATED && j->st == UNINIT)
      return -1;
    if (j->pid == pid)
      return pid;
  }

  // If there's no such pid in jobs, return error.
  return -1;
}

int parse_int(char *str) {
  char *endptr;
  int val = (int)strtol(str, &endptr, 10);
  if (*endptr != '\0')
    return -1;
  return val;
}

void run_fg(pid_t pid) {
  // Enable signal forwarding when FOREGROUND.
  fg_pid = pid;

  /*
   * Because we explicitly wait for this pid, if another background process
   * terminates we won't mistakenly reap it here instead of running this child
   * process to completion.
   */
  int status;
  if (waitpid(pid, &status, WUNTRACED) < 0)
    unix_error("waitpid error");

  if (WIFSIGNALED(status)) {
    int jid;
    if ((jid = setjobstat(pid, TERMINATED)) != -1)
      sprintf(sigbuf, "Job [%d] %d terminated by signal", jid, pid);
    else
      sprintf(sigbuf, "Job [-] %d terminated by signal", pid);
    psignal(WTERMSIG(status), sigbuf);
  } else if (WIFSTOPPED(status)) {
    fg_pid = 0;

    int jid;
    if ((jid = addjob(pid, STOPPED)) < 0) {
      printf("Could not add new job\n");
      exit(0);
    }

    // For children terminated by Ctrl+C, clear fg_pid so we avoid sending the
    // same signal again to a terminated process. Since the child is already
    // terminated the shell will crash.
    //
    // For children stopped by Ctrl+Z, clear fg_pid so we avoid sending SIGTSTP
    // several times to an already stopped process, and also add them to jobs
    // list as STOPPED. The shell won't crash but it's not necessary to send
    // that signal over and over again if the child is already suspended.
    //
    // For children that terminate normally also we need to clear fg_pid to
    // avoid sending signals to a terminated process.
    fg_pid = 0;

    sprintf(sigbuf, "Job [%d] %d stopped by signal", jid, pid);
    psignal(WSTOPSIG(status), sigbuf);
  }
}

void run_bg(pid_t pid, char *cmd) {
  // Disable signal forwarding when BACKGROUND.
  fg_pid = 0;

  int jid;
  if ((jid = addjob(pid, RUNNING)) < 0) {
    printf("Could not add new job\n");
    exit(0);
  }
  printf("[%d] %d %s\n", jid, pid, cmd);
}

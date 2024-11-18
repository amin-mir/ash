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

// TODO: Reap children and update the job status to terminated.

// Returns -1 when there are no empty slots to add the new job.
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

void forward_signal(int sig) {
  // Only forward if there is a foreground process, otherwise
  // ignore the signal.
  if (fg_pid) {
    printf("forwarding sig %d to %d\n", sig, fg_pid);
    if (kill(fg_pid, sig) < 0) {
      unix_error("Forward signal error");
    }

    Status st;
    if (sig == SIGINT) {
      st = TERMINATED;
    } else {
      st = STOPPED;
    }
    addjob(fg_pid, st);
  }
}

// TODO: synchronize signal handling (p770-780).
void reap_child(int sig) {
  printf("reap child handler");
  // int status;
  // pid_t pid = waitpid(-1, &status, 0);
  // if (errno != ECHILD) {
  //   unix_error("waitpid error");
  // }
}

pid_t Fork() {
  pid_t pid;

  if ((pid = fork()) < 0)
    unix_error("Fork error");
  return pid;
}

int main() {
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

    eval(cmdline);
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
        unix_error("Forward signal error");
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
      if (kill(stp_pid, SIGCONT) < 0) {
        unix_error("Forward signal error");
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
  printf("argv[0] %s\n", argv[0]);
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
   * terminates we wont mistakenly reap it here instead of running this child
   * process to completion.
   */
  int status;
  if (waitpid(pid, &status, WUNTRACED) < 0)
    unix_error("waitpid error");

  if (WIFSIGNALED(status)) {
    // Because command was run in foreground it doesn't have a job ID.
    sprintf(sigbuf, "Job [-] %d terminated by signal", pid);
    psignal(WTERMSIG(status), sigbuf);
  } else if (WIFSTOPPED(status)) {
    // Because command was run in foreground it doesn't have a job ID.
    sprintf(sigbuf, "Job [-] %d stopped by signal", pid);
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

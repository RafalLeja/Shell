#include "shell.h"

typedef struct proc {
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job {
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */
#ifdef STUDENT
  (void)status;
  (void)pid;

  // Block SIGCHILD while we are in sigchild_handler
  // This might be unnecessary, but i'm not sure
  sigset_t mask, old_mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  Sigprocmask(SIG_BLOCK, &mask, &old_mask);

  // we iterate through all jobs
  // if the job is not finished, we iterate through all processes in the job
  // if the job or process is finished, we skip it
  // we call waitpid with WNOHANG | WUNTRACED | WCONTINUED to check the status
  // of the process we mark the process acordingly we check if all processes in
  // the job are finished, if so, we mark the job as finished
  for (int j = 0; j < njobmax; j++) {
    // if slot is empty or job is finished, we skip it
    if (jobs[j].pgid == 0 || jobs[j].state == FINISHED) {
      continue;
    }

    // start counting unfinished processes
    int procs_unfinished = 0;
    for (int p = 0; p < jobs[j].nproc; p++) {
      // if process is finished, we skip it
      if (jobs[j].proc[p].state == FINISHED) {
        continue;
      }

      pid =
        Waitpid(jobs[j].proc[p].pid, &status, WNOHANG | WUNTRACED | WCONTINUED);

      switch (pid) {
        case -1:
          // error, this process is finished or dosen't exist
          msg("sigchld_handler: waitpid failed\n");
          return;
          break;

        case 0:
          // this process didn't change state but it cant be finished
          procs_unfinished++;
          break;

        default:
          // this process changed state
          // if the process exited or was killed by a signal, we mark it as
          // finished
          if (WIFEXITED(status) || WIFSIGNALED(status)) {
            jobs[j].proc[p].state = FINISHED;
            jobs[j].proc[p].exitcode = status;
            // if the process was stopped
          } else if (WIFSTOPPED(status)) {
            jobs[j].proc[p].state = STOPPED;
            jobs[j].state = STOPPED;
            procs_unfinished++;
            // if the process was continued
          } else if (WIFCONTINUED(status)) {
            jobs[j].proc[p].state = RUNNING;
            jobs[j].state = RUNNING;
            procs_unfinished++;
          }
          break;
      }
    }

    // if all processes in the job are finished, mark the job as finished
    if (procs_unfinished == 0) {
      jobs[j].state = FINISHED;
    }
  }

  Sigprocmask(SIG_SETMASK, &old_mask, NULL);
#endif /* !STUDENT */
  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job) {
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void) {
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
static int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
#ifdef STUDENT
  (void)exitcode;

  *statusp = exitcode(job);
  // if the job is finished, delete it
  if (state == FINISHED) {
    deljob(job);
  }
#endif /* !STUDENT */

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

    /* TODO: Continue stopped job. Possibly move job to foreground slot. */
#ifdef STUDENT
  (void)movejob;

  // if the job was stopped we need to continue it
  if (bg == FG) {

    movejob(j, FG);

    msg("continue '%s'\n", jobs[FG].command);

    monitorjob(mask);
  } else if (jobs[j].state == STOPPED) {
    Kill(-jobs[j].pgid, SIGCONT);
  }

#endif /* !STUDENT */

  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
#ifdef STUDENT
  int state = jobs[j].state;
  // nie zabijamy śpiącego!
  if (state == STOPPED) {
    // we need to attach the terminal to the job
    // because it might recieve SIGTTIN or SIGTTOU
    Tcsetattr(tty_fd, TCSADRAIN, &jobs[j].tmodes);
    setfgpgrp(jobs[j].pgid);
    Kill(-jobs[j].pgid, SIGTERM);
    Kill(-jobs[j].pgid, SIGCONT);
    setfgpgrp(getpgrp());
    Tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes);
  } else {
    Kill(-jobs[j].pgid, SIGTERM);
  }
#endif /* !STUDENT */

  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    if (jobs[j].pgid == 0)
      continue;

      /* TODO: Report job number, state, command and exit code or signal. */
#ifdef STUDENT
    (void)deljob;

    // if we want to see all jobs or the jobs is in the requested state
    if (which == ALL || jobs[j].state == which) {
      // save the cmd because jobstate might delete it
      int status;
      char *cmd = strdup(jobcmd(j));
      int state = jobstate(j, &status);
      // print the message according to the state
      if (state == FINISHED) {
        if (WIFSIGNALED(status)) {
          msg("[%d] killed '%s' by signal %d\n", j, cmd, WTERMSIG(status));
        } else {
          msg("[%d] exited '%s', status=%d\n", j, cmd, WEXITSTATUS(status));
        }
      } else {
        msg("[%d] %s '%s'\n", j,
            jobs[j].state == STOPPED ? "suspended" : "running", cmd);
      }
      free(cmd);
    }
#endif /* !STUDENT */
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask) {
  int exitcode = 0, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
#ifdef STUDENT
  (void)jobstate;
  (void)exitcode;
  (void)state;

  // saving current terminal modes
  Tcgetattr(tty_fd, &shell_tmodes);
  // setting terminal modes of the job
  Tcsetattr(tty_fd, TCSADRAIN, &jobs[FG].tmodes);
  // setting the foreground process group
  setfgpgrp(jobs[FG].pgid);
  // sending SIGCONT to the job to continue it if it recieved SIGTTIN or SIGTTOU
  Kill(-jobs[FG].pgid, SIGCONT);

  while (1) {
    // waiting for change
    Sigsuspend(mask);
    // checking the state of the job
    state = jobstate(FG, &exitcode);
    // if the job is stopped, move it to the background, if it's finished, break
    // the loop
    if (state == STOPPED) {
      // restoring terminal modes
      Tcgetattr(tty_fd, &jobs[FG].tmodes);
      // moving the job to the background
      movejob(0, allocjob());
      break;
    } else if (state == FINISHED) {
      break;
    }
  }
  // restoring shell as the foreground process group
  setfgpgrp(getpgrp());
  // restoring terminal modes
  Tcsetattr(tty_fd, TCSAFLUSH, &shell_tmodes);
#endif /* !STUDENT */

  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void) {
  struct sigaction act = {
    .sa_flags = SA_RESTART,
    .sa_handler = sigchld_handler,
  };

  /* Block SIGINT for the duration of `sigchld_handler`
   * in case `sigint_handler` does something crazy like `longjmp`. */
  sigemptyset(&act.sa_mask);
  sigaddset(&act.sa_mask, SIGINT);
  Sigaction(SIGCHLD, &act, NULL);

  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void) {
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */
#ifdef STUDENT

  for (int j = 0; j < njobmax; j++) {
    // avoid free slots
    if (jobs[j].pgid != 0) {
      while (jobs[j].state != FINISHED) {
        // kill
        killjob(j);
        // wait
        Sigsuspend(&mask);
      }
    }
  }
  watchjobs(ALL);
#endif /* !STUDENT */

  watchjobs(FINISHED);

  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}

/* Sets foreground process group to `pgid`. */
void setfgpgrp(pid_t pgid) {
  Tcsetpgrp(tty_fd, pgid);
}

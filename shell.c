#ifdef READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static void sigint_handler(int sig) {
  /* No-op handler, we just need break read() call with EINTR. */
  (void)sig;
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
#ifdef STUDENT
    (void)mode;
    (void)MaybeClose;

    /* is this token a redirection operator? */
    if (token[i] == T_INPUT || token[i] == T_OUTPUT || token[i] == T_APPEND) {
      mode = token[i];
      continue;
    }

    /* if not, copy and continue */
    if (mode == NULL) {
      token[n++] = token[i];
      continue;
    }

    /* if it is, open the file and set the mode to NULL */
    if (mode == T_INPUT) {
      *inputp = Open(token[i], O_RDONLY, 0);
      mode = NULL;
    } else if (mode == T_OUTPUT) {
      *outputp = Open(token[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      mode = NULL;
    } else if (mode == T_APPEND) {
      *outputp = Open(token[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
      mode = NULL;
    }

#endif /* !STUDENT */
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
#ifdef STUDENT

  // start blocking SIGCONT now, so that the child doesn't receive it before
  // waiting
  sigset_t sigcont_Mask;
  sigaddset(&sigcont_Mask, SIGCONT);
  Sigprocmask(SIG_BLOCK, &sigcont_Mask, NULL);

  pid_t pid = Fork();

  switch (pid) {
    case -1:
      perror("Fork failed in do_job()");
      exit(-1);
      break;

    case 0:
      // **child**
      // set the process group id to the pid
      setpgid(0, 0);

      // pause until SIGCONT is received
      if (!bg) {
        sigset_t pendingMask;
        while (1) {
          sigpending(&pendingMask);
          if (sigismember(&pendingMask, SIGCONT)) {
            break;
          }
        }
      }

      // reset signal mask
      sigset_t blankMask;
      sigemptyset(&blankMask);
      Sigprocmask(SIG_SETMASK, &blankMask, NULL);

      // set the input and output file descriptors and close duplicates
      if (input != -1) {
        dup2(input, STDIN_FILENO);
        MaybeClose(&input);
      }
      if (output != -1) {
        dup2(output, STDOUT_FILENO);
        MaybeClose(&output);
      }

      // reset signal handlers
      Signal(SIGINT, SIG_DFL);
      Signal(SIGTSTP, SIG_DFL);
      Signal(SIGTTIN, SIG_DFL);
      Signal(SIGTTOU, SIG_DFL);

      // execute the command
      external_command(token);

      // hopefully unreachable
      exit(-1);
      break;

    default:
      // **parent**
      // unblock SIGCONT and block SIGCHLD
      Sigprocmask(SIG_SETMASK, &sigchld_mask, NULL);

      // set the process group id to the pid
      setpgid(pid, pid);

      // add the job to the job list
      int job_id = addjob(pid, bg);

      // add the process to the process list
      addproc(job_id, pid, token);

      // close the input and output file descriptors
      MaybeClose(&input);
      MaybeClose(&output);

      // if the command is not in the background, monitor it
      if (!bg) {
        exitcode = monitorjob(&mask);
      } else {
        msg("[%d] running '%s'\n", job_id, jobcmd(job_id));
      }
  }

#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens, bool bg) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
#ifdef STUDENT

  switch (pid) {
    case -1:
      perror("Fork failed in do_stage()");
      exit(-1);
      break;

    case 0:
      // child
      // set the process group id to the pgid
      setpgid(0, pgid);

      // check if command is internal, and run it
      int exitcode = -1;
      if ((exitcode = builtin_command(token)) >= 0) {
        exit(exitcode);
      }

      // set the input and output file descriptors and close duplicates
      if (input != -1) {
        dup2(input, STDIN_FILENO);
        MaybeClose(&input);
      }
      if (output != -1) {
        dup2(output, STDOUT_FILENO);
        MaybeClose(&output);
      }

      // reset signal handlers
      Signal(SIGINT, SIG_DFL);
      Signal(SIGTSTP, SIG_DFL);
      Signal(SIGTTIN, SIG_DFL);
      Signal(SIGTTOU, SIG_DFL);

      // execute the external command
      external_command(token);

      // hopefully unreachable
      exit(-1);

    default:
      // parent
      // set the process group id to the pgid
      setpgid(pid, pgid);
      break;
  }

#endif /* !STUDENT */

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
#ifdef STUDENT
  (void)input;
  (void)job;
  (void)pid;
  (void)pgid;
  (void)do_stage;

  // Closing pipe because i don't need it now
  MaybeClose(&next_input);
  MaybeClose(&output);

  // initialize variables required for the loop
  int i = 0;
  int stages = 0;
  token_t *queue = calloc(ntokens + 1, sizeof(token_t));
  int queue_size = 0;

  // loop through the tokens, save the tokens in a queue until a pipe is found
  // when a pipe is found, create a new process with the queue as command, and
  // pipe as input/output
  for (i = 0; i < ntokens + 1; i++) {
    if (token[i] == T_PIPE || i == ntokens) {
      // end the queue
      queue[queue_size] = '\0';

      // save pipe output as next input
      input = next_input;

      // create a pipe if it's not the last stage
      if (i != ntokens) {
        mkpipe(&next_input, &output);
      }

      // create a new process
      // if it is the first process, set the pgid to the pid
      // if it is the last process, close the output
      if (stages == 0) {
        pgid = pid = do_stage(0, &mask, -1, output, queue, queue_size, bg);
        MaybeClose(&output);
      } else if (i == ntokens) {
        MaybeClose(&output);
        pid = do_stage(pgid, &mask, input, -1, queue, queue_size, bg);
        MaybeClose(&input);
      } else {
        pid = do_stage(pgid, &mask, input, output, queue, queue_size, bg);
        MaybeClose(&input);
        MaybeClose(&output);
      }

      // add the process to the process list
      // if it is the first process, add the job to the job list
      if (stages == 0) {
        job = addjob(pgid, bg);
      }
      addproc(job, pid, queue);

      stages++;
      // reset the queue
      queue_size = 0;
    } else {
      // append the token to the queue
      queue[queue_size++] = token[i];
    }
  }

  // monitor the job
  if (!bg) {
    exitcode = monitorjob(&mask);
  } else {
    msg("[%d] running '%s'\n", job, jobcmd(job));
  }

  // free the queue
  free(queue);
#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

#ifndef READLINE
static char *readline(const char *prompt) {
  static char line[MAXLINE]; /* `readline` is clearly not reentrant! */

  if (write(STDOUT_FILENO, prompt, strlen(prompt))) {};

  line[0] = '\0';

  ssize_t nread = read(STDIN_FILENO, line, MAXLINE);
  if (nread < 0) {
    if (errno != EINTR)
      unix_error("Read error");
    msg("\n");
  } else if (nread == 0) {
    return NULL; /* EOF */
  } else {
    if (line[nread - 1] == '\n')
      line[nread - 1] = '\0';
  }

  return strdup(line);
}
#endif

int main(int argc, char *argv[]) {
  /* `stdin` should be attached to terminal running in canonical mode */
  if (!isatty(STDIN_FILENO))
    app_error("ERROR: Shell can run only in interactive mode!");

#ifdef READLINE
  rl_initialize();
#endif

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  struct sigaction act = {
    .sa_handler = sigint_handler,
    .sa_flags = 0, /* without SA_RESTART read() will return EINTR */
  };
  Sigaction(SIGINT, &act, NULL);

  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  while (true) {
    char *line = readline("# ");

    if (line == NULL)
      break;

    if (strlen(line)) {
#ifdef READLINE
      add_history(line);
#endif
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}

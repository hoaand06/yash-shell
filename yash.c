// -std=c17 sets __STRICT_ANSI__, which makes glibc hide POSIX-only
// declarations (sigaction, tcsetpgrp, WCONTINUED, ...) unless we ask for
// them explicitly. Must come before any system header is included.
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_TOKENS 100   // max # of input tokens
#define MAX_LINE 2048    // max input line length (caps at 2000)
#define MAX_JOBS 20      // at most 20 concurrent jobs

// ---------------------------------------------------------------------------
// job table
// ---------------------------------------------------------------------------
// one job = one command the user typed (a single process, or two processes
// joined by a single pipe). we track the pgid so signals (SIGCONT, SIGSTOP..)
// can be sent to the whole pipeline at once with kill(-pgid, sig).

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

typedef struct {
    int id;                 // job number as shown in `jobs`; 0 means slot is free
    pid_t pgid;              // process group id shared by every process in this job
    pid_t pids[2];           // pid(s) belonging to this job (2nd only used for a pipe)
    int num_pids;
    job_state_t state;
    int background;          // 1 if launched with '&' (or later moved to bg via `bg`)
    char command[MAX_LINE];  // the raw line the user typed, for jobs/fg/bg output
} job_t;

static job_t jobs[MAX_JOBS];

// ---------------------------------------------------------------------------
// terminal / process-group state for job control
// ---------------------------------------------------------------------------
static pid_t shell_pgid;
static int shell_terminal;
static int shell_is_interactive;

// ---------------------------------------------------------------------------
// one parsed command (one side of an optional pipe)
// ---------------------------------------------------------------------------
typedef struct {
    char *argv[MAX_TOKENS];
    char *infile;
    char *outfile;
    char *errfile;
} cmd_t;

// ---------------------------------------------------------------------------
// job table helpers
// ---------------------------------------------------------------------------

static job_t *find_free_slot(void) { // looks for an empty row to put a new job in
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id == 0) return &jobs[i];
    }
    return NULL;
}

// bash rule (not zsh): a new job gets (1 + the highest job number currently
// in the table), not (count of jobs + 1). this is why job numbers can skip
// (e.g. 1, 2, 4) and later drop back down once the higher-numbered jobs finish.
static int next_job_id(void) {
    int max_id = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id > max_id) max_id = jobs[i].id;
    }
    return max_id + 1;
}

static job_t *highest_id_job(void) {
    job_t *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id != 0 && (!best || jobs[i].id > best->id)) best = &jobs[i];
    }
    return best;
}

static job_t *most_recent_stopped_job(void) {
    job_t *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id != 0 && jobs[i].state == JOB_STOPPED &&
            (!best || jobs[i].id > best->id)) {
            best = &jobs[i];
        }
    }
    return best;
}

static void remove_job(job_t *j) {
    j->id = 0;
}

static const char *state_name(job_state_t s) {
    switch (s) {
        case JOB_RUNNING: return "Running";
        case JOB_STOPPED: return "Stopped";
        case JOB_DONE:    return "Done";
    }
    return "?";
}

// every job gets a '-' except the job with the highest job number,
// which gets a '+' (regardless of whether it's running, stopped, or the one
// that just finished). intentionally simpler than real bash.
static void print_job_line(job_t *j) {
    job_t *top = highest_id_job();
    char marker = (top == j) ? '+' : '-';

    int already_has_amp = 0;
    size_t len = strlen(j->command);
    if (len > 0 && j->command[len - 1] == '&') already_has_amp = 1;

    printf("[%d]%c  %-9s%s%s\n", j->id, marker, state_name(j->state), j->command,
           (j->background && !already_has_amp) ? " &" : "");
}

// ---------------------------------------------------------------------------
// SIGCHLD handling
// ---------------------------------------------------------------------------
// never call waitpid() to reap a specific pid ourselves; the handler below
// is the ONLY place that reaps children. Foreground waits (in launch_pipeline
// or builtin_fg) just spin on sigsuspend() until the job's state field changes,
// which avoids a race between "child exits" and "parent calls waitpid".

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].id == 0) continue;
            for (int k = 0; k < jobs[i].num_pids; k++) {
                if (jobs[i].pids[k] != pid) continue;

                if (WIFSTOPPED(status)) { // job stopped
                    jobs[i].state = JOB_STOPPED;
                } else if (WIFCONTINUED(status)) { // job resumed
                    jobs[i].state = JOB_RUNNING;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) { // job finished
                    jobs[i].pids[k] = -1; // this process is fully reaped
                    int all_reaped = 1;
                    for (int m = 0; m < jobs[i].num_pids; m++) {
                        if (jobs[i].pids[m] != -1) all_reaped = 0;
                    }
                    if (all_reaped) jobs[i].state = JOB_DONE;
                }
                goto next_pid;
            }
        }
    next_pid:;
    }

    errno = saved_errno;
}

// print "Done" for any background job that finished, then drop it from the
// table. foreground jobs are handled inline where they're launched and never
// get a "Done" announcement (background-only).
static void reap_and_report(void) {
    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &prev);

    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id != 0 && jobs[i].background && jobs[i].state == JOB_DONE) {
            print_job_line(&jobs[i]);
            remove_job(&jobs[i]);
        }
    }

    sigprocmask(SIG_SETMASK, &prev, NULL);
}

// ---------------------------------------------------------------------------
// shell setup
// ---------------------------------------------------------------------------

static void init_shell(void) {
    shell_terminal = STDIN_FILENO;
    shell_is_interactive = isatty(shell_terminal);

    if (shell_is_interactive) {
        // make sure we're in the foreground before messing with pgrps.
        while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp())) {
            kill(-shell_pgid, SIGTTIN);
        }

        // the shell itself must ignore these; terminal-generated signals
        // (Ctrl-C, Ctrl-Z) instead go to whichever process group currently
        // owns the terminal (the foreground job), not to us.
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);

        shell_pgid = getpid();
        if (setpgid(shell_pgid, shell_pgid) < 0) {
            perror("yash: couldn't put shell in its own process group");
            exit(EXIT_FAILURE);
        }
        tcsetpgrp(shell_terminal, shell_pgid);
    }

    // let readline run in a straightforward line-at-a-time mode; we handle
    // SIGINT/SIGTSTP ourselves rather than through readline's own handlers.
    rl_catch_signals = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
}

// a forked child must stop ignoring/inheriting the shell's job-control
// signal dispositions, otherwise Ctrl-C would do nothing to it.
static void reset_child_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// fills in cmd->argv (redirection tokens stripped out) and the redirection
// filenames from tokens[0..count).
static void parse_cmd(char **tokens, int count, cmd_t *cmd) {
    memset(cmd, 0, sizeof(*cmd));
    int argi = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(tokens[i], "<") == 0 && i + 1 < count) {
            cmd->infile = tokens[++i];
        } else if (strcmp(tokens[i], ">") == 0 && i + 1 < count) {
            cmd->outfile = tokens[++i];
        } else if (strcmp(tokens[i], "2>") == 0 && i + 1 < count) {
            cmd->errfile = tokens[++i];
        } else if (argi < MAX_TOKENS - 1) {
            cmd->argv[argi++] = tokens[i];
        }
    }
    cmd->argv[argi] = NULL;
}

// Applies <, >, 2> for this command. Must be called in the child, after fork,
// before execvp. Exits the child on failure (e.g. missing input file), which
// satisfies "fail command if input redirection file does not exist".
static void apply_redirections(cmd_t *cmd) {
    if (cmd->infile) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) exit(EXIT_FAILURE);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (cmd->outfile) {
        int fd = open(cmd->outfile, O_WRONLY | O_CREAT | O_TRUNC,
                      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
        if (fd < 0) exit(EXIT_FAILURE);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (cmd->errfile) {
        int fd = open(cmd->errfile, O_WRONLY | O_CREAT | O_TRUNC,
                      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
        if (fd < 0) exit(EXIT_FAILURE);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
}

// ---------------------------------------------------------------------------
// Launching a command (one process, or two joined by a pipe)
// ---------------------------------------------------------------------------

static void launch_pipeline(cmd_t *cmd1, cmd_t *cmd2, int background, const char *raw_line) {
    if (cmd1->argv[0] == NULL || (cmd2 && cmd2->argv[0] == NULL)) {
        return; // malformed line; treat like any other invalid command
    }

    // Block SIGCHLD until the job is fully registered in the table, so the
    // handler can never reap a fast-exiting child before we've recorded it.
    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &prev);

    int pipefd[2] = {-1, -1};
    if (cmd2 && pipe(pipefd) < 0) {
        perror("pipe");
        sigprocmask(SIG_SETMASK, &prev, NULL);
        return;
    }

    pid_t pid1 = fork();
    if (pid1 < 0) {
        perror("fork");
        sigprocmask(SIG_SETMASK, &prev, NULL);
        return;
    }
    if (pid1 == 0) {
        reset_child_signals();
        pid_t self = getpid();
        setpgid(self, self); // first process in the job founds the group
        if (shell_is_interactive && !background) tcsetpgrp(shell_terminal, self);

        if (cmd2) {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }
        apply_redirections(cmd1);
        execvp(cmd1->argv[0], cmd1->argv);
        exit(EXIT_FAILURE); // execvp only returns on failure
    }

    // Parent also sets the pgid (redundant with the child doing it -- this
    // is the standard race-free pattern: whichever of the two runs first
    // "wins", and both are trying to establish the same result).
    setpgid(pid1, pid1);
    pid_t pgid = pid1;
    if (shell_is_interactive && !background) tcsetpgrp(shell_terminal, pgid);

    pid_t pid2 = -1;
    if (cmd2) {
        pid2 = fork();
        if (pid2 == 0) {
            reset_child_signals();
            setpgid(getpid(), pgid);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            apply_redirections(cmd2);
            execvp(cmd2->argv[0], cmd2->argv);
            exit(EXIT_FAILURE);
        }
        setpgid(pid2, pgid);
        close(pipefd[0]);
        close(pipefd[1]);
    }

    job_t *job = find_free_slot();
    if (job) {
        job->id = next_job_id();
        job->pgid = pgid;
        job->pids[0] = pid1;
        job->num_pids = 1;
        if (pid2 > 0) {
            job->pids[1] = pid2;
            job->num_pids = 2;
        }
        job->state = JOB_RUNNING;
        job->background = background;
        strncpy(job->command, raw_line, MAX_LINE - 1);
        job->command[MAX_LINE - 1] = '\0';
    }

    if (!background) {
        // Wait for the job to stop or finish. sigsuspend() atomically
        // unblocks SIGCHLD and sleeps, so we can't miss the signal between
        // checking job->state and actually going to sleep.
        while (job && job->state == JOB_RUNNING) {
            sigsuspend(&prev);
        }
        if (shell_is_interactive) tcsetpgrp(shell_terminal, shell_pgid);
        if (job && job->state == JOB_DONE) remove_job(job);
        // if stopped, it stays in the table for jobs/fg/bg
    }

    sigprocmask(SIG_SETMASK, &prev, NULL);
}

// ---------------------------------------------------------------------------
// Builtins: jobs, fg, bg
// ---------------------------------------------------------------------------

static void builtin_jobs(void) {
    job_t *list[MAX_JOBS];
    int count = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].id != 0) list[count++] = &jobs[i];
    }
    // insertion sort by job id (count <= 20, so this is plenty fast)
    for (int i = 1; i < count; i++) {
        job_t *key = list[i];
        int j = i - 1;
        while (j >= 0 && list[j]->id > key->id) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
    for (int i = 0; i < count; i++) print_job_line(list[i]);
}

static void builtin_fg(void) {
    job_t *job = highest_id_job();
    if (!job) return; // no jobs to bring to the foreground

    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &prev);

    printf("%s\n", job->command); // bash echoes the command line for fg

    if (job->state == JOB_STOPPED) kill(-job->pgid, SIGCONT);
    job->state = JOB_RUNNING;
    job->background = 0;
    if (shell_is_interactive) tcsetpgrp(shell_terminal, job->pgid);

    while (job->state == JOB_RUNNING) {
        sigsuspend(&prev);
    }

    if (shell_is_interactive) tcsetpgrp(shell_terminal, shell_pgid);
    if (job->state == JOB_DONE) remove_job(job);

    sigprocmask(SIG_SETMASK, &prev, NULL);
}

static void builtin_bg(void) {
    job_t *job = most_recent_stopped_job();
    if (!job) return; // no stopped job to resume

    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &prev);

    kill(-job->pgid, SIGCONT);
    job->state = JOB_RUNNING;
    job->background = 1;
    print_job_line(job); // bash prints the resumed job in `jobs` format + " &"

    sigprocmask(SIG_SETMASK, &prev, NULL);
}

// ---------------------------------------------------------------------------
// Top-level line processing
// ---------------------------------------------------------------------------

static void process_line(char *line) {
    // Keep an untouched copy for the job table before strtok() chops up
    // `line` in place with '\0's.
    char raw[MAX_LINE];
    strncpy(raw, line, MAX_LINE - 1);
    raw[MAX_LINE - 1] = '\0';

    char *tokens[MAX_TOKENS];
    int n = 0;
    char *tok = strtok(line, " ");
    while (tok != NULL && n < MAX_TOKENS - 1) {
        tokens[n++] = tok;
        tok = strtok(NULL, " ");
    }
    tokens[n] = NULL;
    if (n == 0) return;

    int background = 0;
    if (strcmp(tokens[n - 1], "&") == 0) {
        background = 1;
        tokens[--n] = NULL;
    }

    if (strcmp(tokens[0], "jobs") == 0) { builtin_jobs(); return; }
    if (strcmp(tokens[0], "fg") == 0)   { builtin_fg();   return; }
    if (strcmp(tokens[0], "bg") == 0)   { builtin_bg();   return; }

    int pipe_idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(tokens[i], "|") == 0) { pipe_idx = i; break; }
    }

    cmd_t cmd1, cmd2;
    if (pipe_idx != -1) {
        parse_cmd(tokens, pipe_idx, &cmd1);
        parse_cmd(tokens + pipe_idx + 1, n - pipe_idx - 1, &cmd2);
        launch_pipeline(&cmd1, &cmd2, background, raw);
    } else {
        parse_cmd(tokens, n, &cmd1);
        launch_pipeline(&cmd1, NULL, background, raw);
    }
}

int main(void) {
    init_shell();

    char *line;
    while (1) {
        line = readline("# ");

        if (line == NULL) { // Ctrl-D
            printf("\n");
            break;
        }

        // Announce any background jobs that finished since the last prompt.
        reap_and_report();

        if (strlen(line) > 0) {
            process_line(line);
        }

        free(line);
    }

    return 0;
}

/*
 * compat_proc.c — Portable bidirectional child-process pipe.
 *
 * POSIX: pipe(2) x2 + fork + dup2 + execvp.
 * Windows: CreatePipe x2 + CreateProcessW + _open_osfhandle.
 *
 * See compat_proc.h for the contract. Line-oriented request/response over a
 * persistent child; the child's stderr is inherited from the parent.
 */
#include "foundation/compat_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "foundation/win_utf8.h"

struct cbm_proc {
    HANDLE child; /* process handle */
    HANDLE in_w;  /* write end -> child's stdin */
    HANDLE out_r; /* read end  <- child's stdout */
    char *rbuf;   /* read buffer for line assembly */
    size_t rlen;  /* bytes currently in rbuf */
    size_t rcap;  /* capacity of rbuf */
};

/* Build a CreateProcess command line from argv with minimal quoting: each arg
 * containing a space or quote is wrapped in double-quotes with embedded quotes
 * doubled. Adequate for our own argv (python + script path + flags). */
static wchar_t *build_cmdline(const char *const *argv) {
    /* Compose a UTF-8 command line first, then widen. */
    size_t cap = 256, len = 0;
    char *cmd = (char *)malloc(cap);
    if (!cmd)
        return NULL;
    cmd[0] = '\0';
    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];
        bool need_q = (a[0] == '\0') || strpbrk(a, " \t\"") != NULL;
        size_t need = strlen(a) * 2 + 4;
        if (len + need + 2 >= cap) {
            while (len + need + 2 >= cap)
                cap *= 2;
            char *n = (char *)realloc(cmd, cap);
            if (!n) {
                free(cmd);
                return NULL;
            }
            cmd = n;
        }
        if (i > 0)
            cmd[len++] = ' ';
        if (need_q)
            cmd[len++] = '"';
        for (const char *p = a; *p; p++) {
            if (*p == '"')
                cmd[len++] = '"'; /* double embedded quotes */
            cmd[len++] = *p;
        }
        if (need_q)
            cmd[len++] = '"';
        cmd[len] = '\0';
    }
    wchar_t *w = cbm_utf8_to_wide(cmd);
    free(cmd);
    return w;
}

cbm_proc_t *cbm_proc_spawn(const char *const *argv) {
    if (!argv || !argv[0])
        return NULL;

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0))
        return NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        return NULL;
    }
    /* The parent's own ends must NOT be inherited by the child. */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    wchar_t *cmdline = build_cmdline(argv);
    if (!cmdline) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(out_w);
        return NULL;
    }

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE); /* inherit parent's stderr */
    PROCESS_INFORMATION pi = {0};

    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmdline);
    /* Child owns these ends now; close the parent copies regardless. */
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        return NULL;
    }
    CloseHandle(pi.hThread);

    cbm_proc_t *p = (cbm_proc_t *)calloc(1, sizeof(*p));
    if (!p) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return NULL;
    }
    p->child = pi.hProcess;
    p->in_w = in_w;
    p->out_r = out_r;
    p->rcap = 8192;
    p->rbuf = (char *)malloc(p->rcap);
    if (!p->rbuf) {
        cbm_proc_close(p);
        return NULL;
    }
    return p;
}

int cbm_proc_write_line(cbm_proc_t *p, const char *line) {
    if (!p || !p->in_w || !line)
        return 1;
    size_t n = strlen(line);
    DWORD wrote = 0;
    if (n > 0 && !WriteFile(p->in_w, line, (DWORD)n, &wrote, NULL))
        return 1;
    if (n == 0 || line[n - 1] != '\n') {
        char nl = '\n';
        if (!WriteFile(p->in_w, &nl, 1, &wrote, NULL))
            return 1;
    }
    return 0;
}

long cbm_proc_read_line(cbm_proc_t *p, char *buf, size_t buf_sz) {
    if (!p || !p->out_r || !buf || buf_sz == 0)
        return -1;
    for (;;) {
        /* Is there a newline already buffered? */
        for (size_t i = 0; i < p->rlen; i++) {
            if (p->rbuf[i] == '\n') {
                size_t linelen = i;
                size_t copy = linelen < buf_sz - 1 ? linelen : buf_sz - 1;
                memcpy(buf, p->rbuf, copy);
                buf[copy] = '\0';
                /* shift remainder down */
                size_t rest = p->rlen - (i + 1);
                memmove(p->rbuf, p->rbuf + i + 1, rest);
                p->rlen = rest;
                return (long)copy;
            }
        }
        /* Need more data. Grow buffer if full. */
        if (p->rlen == p->rcap) {
            size_t ncap = p->rcap * 2;
            char *nb = (char *)realloc(p->rbuf, ncap);
            if (!nb)
                return -1;
            p->rbuf = nb;
            p->rcap = ncap;
        }
        DWORD got = 0;
        if (!ReadFile(p->out_r, p->rbuf + p->rlen, (DWORD)(p->rcap - p->rlen), &got, NULL) ||
            got == 0) {
            /* EOF or error: flush any trailing partial line. */
            if (p->rlen > 0) {
                size_t copy = p->rlen < buf_sz - 1 ? p->rlen : buf_sz - 1;
                memcpy(buf, p->rbuf, copy);
                buf[copy] = '\0';
                p->rlen = 0;
                return (long)copy;
            }
            return -1;
        }
        p->rlen += got;
    }
}

void cbm_proc_close(cbm_proc_t *p) {
    if (!p)
        return;
    if (p->in_w)
        CloseHandle(p->in_w);
    if (p->out_r)
        CloseHandle(p->out_r);
    if (p->child) {
        /* Closing stdin should let a well-behaved child exit; give it a beat,
         * then terminate. */
        if (WaitForSingleObject(p->child, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(p->child, 1);
        }
        CloseHandle(p->child);
    }
    free(p->rbuf);
    free(p);
}

#else /* POSIX */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct cbm_proc {
    pid_t child;
    int in_w;  /* write -> child stdin */
    int out_r; /* read  <- child stdout */
    char *rbuf;
    size_t rlen;
    size_t rcap;
};

cbm_proc_t *cbm_proc_spawn(const char *const *argv) {
    if (!argv || !argv[0])
        return NULL;
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0)
        return NULL;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return NULL;
    }
    if (pid == 0) {
        /* Child: stdin <- in_pipe[0], stdout -> out_pipe[1]. */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        /* Close all pipe fds (originals + dups beyond the std ones). */
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127); /* exec failed */
    }
    /* Parent: keep in_w (write) and out_r (read), close child ends. */
    close(in_pipe[0]);
    close(out_pipe[1]);

    cbm_proc_t *p = (cbm_proc_t *)calloc(1, sizeof(*p));
    if (!p) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    p->child = pid;
    p->in_w = in_pipe[1];
    p->out_r = out_pipe[0];
    p->rcap = 8192;
    p->rbuf = (char *)malloc(p->rcap);
    if (!p->rbuf) {
        cbm_proc_close(p);
        return NULL;
    }
    return p;
}

int cbm_proc_write_line(cbm_proc_t *p, const char *line) {
    if (!p || p->in_w < 0 || !line)
        return 1;
    size_t n = strlen(line);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(p->in_w, line + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        off += (size_t)w;
    }
    if (n == 0 || line[n - 1] != '\n') {
        char nl = '\n';
        ssize_t w;
        do {
            w = write(p->in_w, &nl, 1);
        } while (w < 0 && errno == EINTR);
        if (w < 0)
            return 1;
    }
    return 0;
}

long cbm_proc_read_line(cbm_proc_t *p, char *buf, size_t buf_sz) {
    if (!p || p->out_r < 0 || !buf || buf_sz == 0)
        return -1;
    for (;;) {
        for (size_t i = 0; i < p->rlen; i++) {
            if (p->rbuf[i] == '\n') {
                size_t copy = i < buf_sz - 1 ? i : buf_sz - 1;
                memcpy(buf, p->rbuf, copy);
                buf[copy] = '\0';
                size_t rest = p->rlen - (i + 1);
                memmove(p->rbuf, p->rbuf + i + 1, rest);
                p->rlen = rest;
                return (long)copy;
            }
        }
        if (p->rlen == p->rcap) {
            size_t ncap = p->rcap * 2;
            char *nb = (char *)realloc(p->rbuf, ncap);
            if (!nb)
                return -1;
            p->rbuf = nb;
            p->rcap = ncap;
        }
        ssize_t got = read(p->out_r, p->rbuf + p->rlen, p->rcap - p->rlen);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0) {
            if (p->rlen > 0) {
                size_t copy = p->rlen < buf_sz - 1 ? p->rlen : buf_sz - 1;
                memcpy(buf, p->rbuf, copy);
                buf[copy] = '\0';
                p->rlen = 0;
                return (long)copy;
            }
            return -1;
        }
        p->rlen += (size_t)got;
    }
}

void cbm_proc_close(cbm_proc_t *p) {
    if (!p)
        return;
    if (p->in_w >= 0)
        close(p->in_w); /* EOF on child's stdin */
    if (p->out_r >= 0)
        close(p->out_r);
    if (p->child > 0) {
        /* Reap; if it lingers, SIGKILL. */
        for (int i = 0; i < 20; i++) {
            int st;
            pid_t r = waitpid(p->child, &st, WNOHANG);
            if (r == p->child || r < 0) {
                p->child = 0;
                break;
            }
            struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
            nanosleep(&ts, NULL);
        }
        if (p->child > 0) {
            kill(p->child, SIGKILL);
            waitpid(p->child, NULL, 0);
        }
    }
    free(p->rbuf);
    free(p);
}

#endif

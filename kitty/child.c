#include "data-types.h"
#include "safe-wrappers.h"
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>

#define EXTRA_ENV_BUFFER_SIZE 64

static char ** serialize_string_tuple(PyObject *src, Py_ssize_t extra) {
    const Py_ssize_t sz = PyTuple_GET_SIZE(src);
    size_t required_size = sizeof(char *) * (sz + extra + 1);
    void *block = calloc(required_size, 1);
    if (!block) { PyErr_NoMemory(); return NULL; }
    char **ans = block;
    for (Py_ssize_t i = 0; i < sz; i++) {
        PyObject *x = PyTuple_GET_ITEM(src, i);
        if (!PyUnicode_Check(x)) { free(block); PyErr_SetString(PyExc_TypeError, "string tuple must have only strings"); return NULL; }
        ans[i] = (char *)PyUnicode_AsUTF8(x);
        if (!ans[i]) { free(block); return NULL; }
    }
    return ans;
}

static void write_to_stderr(const char *text) {
    size_t sz = strlen(text), written = 0;
    while (written < sz) {
        ssize_t amt = write(2, text + written, sz - written);
        if (amt == 0) break;
        if (amt < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
        written += amt;
    }
}

#define exit_on_err(m) { write_to_stderr(m); write_to_stderr(": "); write_to_stderr(strerror(errno)); write_to_stderr("\n"); exit(EXIT_FAILURE); }

static void wait_for_terminal_ready(int fd) {
    char data;
    while (1) { if (read(fd, &data, 1) == -1 && (errno == EINTR || errno == EAGAIN)) continue; break; }
}

static void close_all_but(int keep_open[], size_t keep_count) {
    int max_fd = 256;
#if defined(__linux__) && defined(CLOSE_RANGE_CLOEXEC)
    if (close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) == 0) {
        for (size_t i = 0; i < keep_count; i++) {
            int fd = keep_open[i];
            if (fd >= 0) {
                int flags = fcntl(fd, F_GETFD, 0);
                if (flags >= 0) { flags &= ~FD_CLOEXEC; fcntl(fd, F_SETFD, flags); }
            }
        }
        return;
    }
#endif
    for (int fd = 3; fd < max_fd; fd++) {
        int keep = 0;
        for (size_t i = 0; i < keep_count; i++) { if (keep_open[i] == fd) { keep = 1; break; } }
        if (!keep) close(fd);
    }
}

static PyObject * spawn(PyObject *self UNUSED, PyObject *args) {
    PyObject *argv_p, *env_p, *handled_signals_p, *pass_fds;
    int master, slave, stdin_read_fd, stdin_write_fd, ready_read_fd, ready_write_fd, forward_stdio;
    const char *kitten_exe; char *cwd, *exe;
    if (!PyArg_ParseTuple(args, "ssO!O!iiiiiiO!spO!", &exe, &cwd, &PyTuple_Type, &argv_p, &PyTuple_Type, &env_p, &master, &slave, &stdin_read_fd, &stdin_write_fd, &ready_read_fd, &ready_write_fd, &PyTuple_Type, &handled_signals_p, &kitten_exe, &forward_stdio, &PyTuple_Type, &pass_fds)) return NULL;
    char name[2048] = {0};
    if (ttyname_r(slave, name, sizeof(name) - 1) != 0) { PyErr_SetFromErrno(PyExc_OSError); return NULL; }
    char **argv = serialize_string_tuple(argv_p, 0); if (!argv) return NULL;
    char **env = serialize_string_tuple(env_p, 1); if (!env) { free(argv); return NULL; }
    int handled_signals[16] = {0};
    int num_handled_signals = MIN((int)arraysz(handled_signals), (int)PyTuple_GET_SIZE(handled_signals_p));
    for (Py_ssize_t i = 0; i < num_handled_signals; i++) handled_signals[i] = PyLong_AsLong(PyTuple_GET_ITEM(handled_signals_p, i));
#if PY_VERSION_HEX >= 0x03070000
    PyOS_BeforeFork();
#endif
    pid_t pid = fork();
    switch (pid) {
        case 0: { // child
#if PY_VERSION_HEX >= 0x03070000
            PyOS_AfterFork_Child();
#endif
            const struct sigaction act = {.sa_handler = SIG_DFL};
#define SA(which) if (sigaction(which, &act, NULL) != 0) exit_on_err("sigaction() in child process failed");
            for (int si = 0; si < num_handled_signals; si++) { SA(handled_signals[si]); }
#ifdef SIGPIPE
            SA(SIGPIPE);
#endif
#ifdef SIGXFSZ
            SA(SIGXFSZ);
#endif
#undef SA
            sigset_t signals; sigemptyset(&signals);
            if (sigprocmask(SIG_SETMASK, &signals, NULL) != 0) exit_on_err("sigprocmask() in child process failed");
            if (chdir(cwd) != 0) { if (access(".", X_OK) != 0) { if (chdir("/") != 0) {} } }
            if (setsid() == -1) exit_on_err("setsid() in child process failed");
            int tfd = safe_open(name, O_RDWR | O_CLOEXEC, 0);
            if (tfd == -1) exit_on_err("Failed to open controlling terminal");
            if (ioctl(tfd, TIOCSCTTY, 0) == -1) exit_on_err("Failed to set controlling terminal with TIOCSCTTY");
            safe_close(tfd, __FILE__, __LINE__);
            int preserve[16]; size_t preserve_count = 0;
            if (forward_stdio) {
                int fd = safe_dup(STDOUT_FILENO); if (fd < 0) exit_on_err("dup() failed for forwarded STDOUT");
                preserve[preserve_count++] = fd; size_t s = PyTuple_GET_SIZE(env_p);
                env[s] = malloc(EXTRA_ENV_BUFFER_SIZE); if (!env[s]) exit_on_err("Failed to allocate environment buffer block");
                snprintf(env[s], EXTRA_ENV_BUFFER_SIZE, "KITTY_STDIO_FORWARDED=%d", fd); env[s + 1] = NULL;
                fd = safe_dup(STDERR_FILENO); if (fd < 0) exit_on_err("dup() failed for forwarded STDERR");
                preserve[preserve_count++] = fd;
            } else { size_t s = PyTuple_GET_SIZE(env_p); env[s] = NULL; }
            for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(pass_fds); i++) {
                PyObject *pfd = PyTuple_GET_ITEM(pass_fds, i); if (!PyLong_Check(pfd)) exit_on_err("pass_fds must contain only integers");
                int fd = PyLong_AsLong(pfd); if (fd > -1 && fd < FD_SETSIZE && preserve_count < arraysz(preserve)) { preserve[preserve_count++] = fd; }
            }
            if (safe_dup2(slave, STDOUT_FILENO) == -1) exit_on_err("dup2() failed for fd 1");
            if (safe_dup2(slave, STDERR_FILENO) == -1) exit_on_err("dup2() failed for fd 2");
            if (stdin_read_fd > -1) {
                if (safe_dup2(stdin_read_fd, STDIN_FILENO) == -1) exit_on_err("dup2() failed for fd 0");
                safe_close(stdin_read_fd, __FILE__, __LINE__); safe_close(stdin_write_fd, __FILE__, __LINE__);
            } else { if (safe_dup2(slave, STDIN_FILENO) == -1) exit_on_err("dup2() failed for fd 0"); }
            safe_close(slave, __FILE__, __LINE__); safe_close(master, __FILE__, __LINE__);
            safe_close(ready_write_fd, __FILE__, __LINE__); wait_for_terminal_ready(ready_read_fd); safe_close(ready_read_fd, __FILE__, __LINE__);
            close_all_but(preserve, preserve_count);
            extern char **environ; environ = env; execvp(exe, argv);
            write_to_stderr("Failed to launch child: "); write_to_stderr(exe); write_to_stderr("\nWith error: "); write_to_stderr(strerror(errno)); write_to_stderr("\n");
            execlp(kitten_exe, "kitten", "__hold_till_enter__", NULL); exit(EXIT_FAILURE); break;
        }
        case -1: {
#if PY_VERSION_HEX >= 0x03070000
            int saved_errno = errno; PyOS_AfterFork_Parent(); errno = saved_errno;
#endif
            PyErr_SetFromErrno(PyExc_OSError); break;
        }
        default:
#if PY_VERSION_HEX >= 0x03070000
            PyOS_AfterFork_Parent();
#endif
            break;
    }
    free(argv); if (pid != 0) { free(env); }
    if (PyErr_Occurred()) return NULL;
    return PyLong_FromLong(pid);
}

static PyMethodDef module_methods[] = { METHODB(spawn, METH_VARARGS), {NULL, NULL, 0, NULL} };

bool init_child(PyObject *module) {
    PyModule_AddIntMacro(module, CLD_KILLED); PyModule_AddIntMacro(module, CLD_STOPPED);
    PyModule_AddIntMacro(module, CLD_EXITED); PyModule_AddIntMacro(module, CLD_CONTINUED);
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    return true;
}

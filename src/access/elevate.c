#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/access.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"

static int child_status_to_exit_code(int status);

static int child_status_to_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 3;
}

int elevate_with_sudo_canonical(const char *program, int canonical_argc, char **canonical_argv) {
    char abs_sigmund[SIGMUND_PATH_MAX];
    if (resolve_self_executable_path(program, abs_sigmund, sizeof(abs_sigmund)) != 0) {
        fprintf(stderr, "sigmund: cannot determine executable path for sudo self-elevation\n");
        return 3;
    }

    int argc = 5 + canonical_argc;
    char **sudo_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!sudo_argv) {
        return 3;
    }
    sudo_argv[0] = "sudo";
    sudo_argv[1] = "--";
    sudo_argv[2] = abs_sigmund;
    sudo_argv[3] = "--system";
    sudo_argv[4] = "--elevated";
    for (int i = 0; i < canonical_argc; i++) {
        sudo_argv[5 + i] = canonical_argv[i];
    }
    sudo_argv[argc] = NULL;

    const char *sudo_prog = "sudo";
#ifdef SIGMUND_TESTING
    const char *test_sudo_prog = getenv("SIGMUND_TEST_SUDO_PROG");
    if (test_sudo_prog && *test_sudo_prog) {
        sudo_prog = test_sudo_prog;
    }
#endif

    fflush(NULL);

    struct sigaction ign;
    struct sigaction old_int;
    struct sigaction old_quit;
    bool have_old_int = false;
    bool have_old_quit = false;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &ign, &old_int) == 0) {
        have_old_int = true;
    }
    if (sigaction(SIGQUIT, &ign, &old_quit) == 0) {
        have_old_quit = true;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        free(sudo_argv);
        errno = saved;
        fprintf(stderr, "sigmund: failed to fork sudo: %s\n", strerror(errno));
        return 3;
    }
    if (pid == 0) {
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        execvp(sudo_prog, sudo_argv);
        int saved = errno;
        fprintf(stderr, "sigmund: failed to exec sudo: %s\n", strerror(saved));
        _exit(127);
    }

    free(sudo_argv);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        int saved = errno;
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        errno = saved;
        fprintf(stderr, "sigmund: failed to wait for sudo: %s\n", strerror(errno));
        return 3;
    }

    if (have_old_int) sigaction(SIGINT, &old_int, NULL);
    if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
    return child_status_to_exit_code(status);
}

int elevate_with_sudo_parsed(const char *program,
                                    bool owned,
                                    const char *command,
                                    bool tail,
                                    bool console_mode,
                                    bool all,
                                    bool print_cmd,
                                    bool multi,
                                    int multi_count,
                                    bool force_raw,
                                    int argc,
                                    char **argv) {
    int extra = argc;
    if (owned) {
        extra += 1;
        if (!strcmp(command, "start") && tail) {
            extra += 1;
        }
        if (!strcmp(command, "start") && console_mode) {
            extra += 1;
        }
        if (all) {
            extra += 1;
        }
        if (print_cmd) {
            extra += 1;
        }
        if (!strcmp(command, "start") && multi) {
            extra += multi_count == 1 ? 1 : 2;
        }
    } else {
        if (tail) {
            extra += 1;
        }
        if (console_mode) {
            extra += 1;
        }
        if (force_raw) {
            extra += 1;
        }
    }

    char **canon = calloc((size_t)extra, sizeof(char *));
    if (!canon) {
        return 3;
    }
    char count_buf[32];
    int n = 0;
    if (owned) {
        canon[n++] = (char *)command;
        if (!strcmp(command, "start") && tail) {
            canon[n++] = "--tail";
        }
        if (!strcmp(command, "start") && console_mode) {
            canon[n++] = "--console";
        }
        if (all) {
            canon[n++] = "--all";
        }
        if (print_cmd) {
            canon[n++] = "--print";
        }
        if (!strcmp(command, "start") && multi) {
            canon[n++] = "--multi";
            if (multi_count != 1) {
                snprintf(count_buf, sizeof(count_buf), "%d", multi_count);
                canon[n++] = count_buf;
            }
        }
    } else {
        if (tail) {
            canon[n++] = "--tail";
        }
        if (console_mode) {
            canon[n++] = "--console";
        }
        if (force_raw) {
            canon[n++] = "--";
        }
    }
    for (int i = 0; i < argc; i++) {
        canon[n++] = argv[i];
    }
    int rc = elevate_with_sudo_canonical(program, n, canon);
    free(canon);
    return rc;
}

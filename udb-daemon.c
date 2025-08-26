#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ketopt.h"
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "config.h"

#define UNREACHABLE                                                                                \
    do                                                                                             \
    {                                                                                              \
        fprintf (stderr, "%s:%u Entered unreachable part of code! Aborting.\n", __FILE__,          \
                 __LINE__);                                                                        \
        abort ();                                                                                  \
    } while (0)

typedef struct
{
    ssize_t pfd_idx;
    size_t wlen, woff;
    char wbuf[UDB_MAX_MSG_LEN];
} UDB_ClientContext;

static volatile bool udb_quit = false;
static int udb_sockfd = -1;
static size_t fixed_count = 0;
static struct pollfd *pfds = NULL;
static UDB_ClientContext **clients = NULL;

static ssize_t
udb_add_fixed_fd (int fd, short events)
{
    struct pollfd p = { .fd = fd, .events = events };
    arrput (pfds, p);
    arrput (clients, NULL);
    fixed_count += 1;
    return fixed_count - 1;
}

static ssize_t
udb_client_register_fd (int cfd)
{
    UDB_ClientContext *c = calloc (1, sizeof (UDB_ClientContext));
    if (!c)
        return -1;

    struct pollfd p = { .fd = cfd, .events = POLLIN, .revents = 0 };

    arrput (pfds, p);
    arrput (clients, c);

    ssize_t idx = (ssize_t)arrlen (pfds) - 1;
    c->pfd_idx = idx;
    return idx;
}

static void
udb_client_pollout_set (size_t idx, bool enable)
{
    if (idx < fixed_count || (long)idx >= arrlen (pfds))
        return;
    if (enable)
        pfds[idx].events |= POLLOUT;
    else
        pfds[idx].events &= ~POLLOUT;
}

static ssize_t
udb_client_unregister_idx (size_t idx)
{
    size_t n = arrlen (pfds);
    if (idx < fixed_count || (size_t)idx >= n)
        return -1;

    close (pfds[idx].fd);

    UDB_ClientContext *c = clients[idx];
    if (c)
        free (c);

    size_t last = n - 1;
    if (idx != last)
    {
        pfds[idx] = pfds[last];
        clients[idx] = clients[last];

        if (clients[idx])
            clients[idx]->pfd_idx = idx;
    }

    (void)arrpop (pfds);
    (void)arrpop (clients);

    return 0;
}

static ssize_t
udb_client_read (size_t idx)
{
    fprintf (stdout, "udb_client_read(%lu) called\n", idx);
    return -1;
}

static void
udb_signal_handler (int signo)
{
    (void)signo;
    fprintf (stdout, "\nQuitting...\n");
    udb_quit = true;
}

static char *udb_db_path = UDB_DATABASE_FILE_PATH_DEFAULT;
static char *udb_socket_path = UDB_SOCKET_PATH_DEFAULT;

static int
write_pidfile (const char *path)
{
    mode_t oldmask = umask (0);
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    umask (oldmask);

    if (fd < 0)
    {
        fprintf (stderr, "pidfile creation failed: %s (%s)\n", strerror (errno), path);
        return -1;
    }
    char buf[32];
    int len = snprintf (buf, sizeof buf, "%d\n", (int)getpid ());
    if (write (fd, buf, len) != len)
    {
        fprintf (stderr, "writing pidfile failed: %s\n", strerror (errno));
        close (fd);
        return -1;
    }
    fsync (fd);
    close (fd);
    return 0;
}

static int
make_timerfd (int initial_sec, int interval_sec)
{
    int tfd = timerfd_create (CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
        return tfd;

    struct itimerspec its = { 0 };
    its.it_value.tv_sec = initial_sec;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = interval_sec;
    its.it_interval.tv_nsec = 0;

    if (timerfd_settime (tfd, 0, &its, NULL) < 0)
    {
        close (tfd);
        return -1;
    }

    return tfd;
}

static int
udb_load_from_file (void)
{
    fprintf (stdout, "udb_load_from_file() called\n");
    return 0;
}

static int
udb_save_to_file (void)
{
    fprintf (stdout, "udb_save_to_file() called\n");
    return 0;
}

enum
{
    ARG_HELP = 256,
    ARG_DAEMONIZE,
    ARG_SOCKET_PATH,
    ARG_DB_PATH,
};

static const ko_longopt_t longopts[] = {
    { "help", ko_no_argument, ARG_HELP },
    { "daemonize", ko_no_argument, ARG_DAEMONIZE },
    { "socket-path", ko_required_argument, ARG_SOCKET_PATH },
    { "db-path", ko_required_argument, ARG_DB_PATH },
    { NULL, 0, 0 },
};

static void
usage (const char *progname)
{
    printf ("Usage: %s [FLAGS] [ARGS]\n", progname);
    printf ("FLAGS:\n");
    printf ("\t-h, --help        - display this message\n");
    printf ("\t-d, --daemonize   - daemonize using double-fork method (not recommended - use "
            "systemd like a normal person)\n");
    printf ("ARGS:\n");
    printf ("\t-s, --socket-path - change path to unix socket. DEFAULT: %s\n",
            UDB_SOCKET_PATH_DEFAULT);
    printf ("\t-p, --db-path     - provide path to database file. DEFAULT: %s\n",
            UDB_DATABASE_FILE_PATH_DEFAULT ? UDB_DATABASE_FILE_PATH_DEFAULT
                                           : "NULL (database state not persisted)");
}

static void
daemonize (void)
{
    pid_t pid;

    pid = fork ();
    if (pid < 0)
    {
        fprintf (stderr, "fork() failed: %s\n", strerror (errno));
        exit (EXIT_FAILURE);
    }
    if (pid > 0)
    {
        _exit (EXIT_SUCCESS);
    }

    if (setsid () < 0)
    {
        fprintf (stderr, "setsid() failed: %s\n", strerror (errno));
        exit (EXIT_FAILURE);
    }

    {
        struct sigaction sa;
        sa.sa_handler = SIG_IGN;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction (SIGHUP, &sa, NULL);
    }

    pid = fork ();
    if (pid < 0)
    {
        fprintf (stderr, "second fork() failed: %s\n", strerror (errno));
        exit (EXIT_FAILURE);
    }
    if (pid > 0)
    {
        _exit (EXIT_SUCCESS);
    }

    write_pidfile ("/tmp/udb.pid");

    umask (0);
    if (chdir ("/") < 0)
    {
        fprintf (stderr, "chdir(\"/\") failed: %s\n", strerror (errno));
    }

    {
        struct rlimit rl;
        if (getrlimit (RLIMIT_NOFILE, &rl) == 0)
        {
            for (rlim_t fd = 0; fd < rl.rlim_max; ++fd)
                close ((int)fd);
        }
        else
        {
            long maxfd = sysconf (_SC_OPEN_MAX);
            if (maxfd < 0)
                maxfd = 1024;
            for (int fd = 0; fd < (int)maxfd; ++fd)
                close (fd);
        }
    }

    {
        int fd = open ("/dev/null", O_RDWR);
        if (fd < 0)
        {
            fprintf (stderr, "open(\"/dev/null\") failed: %s\n", strerror (errno));
            exit (EXIT_FAILURE);
        }
        if (dup2 (fd, STDIN_FILENO) < 0 || dup2 (fd, STDOUT_FILENO) < 0
            || dup2 (fd, STDERR_FILENO) < 0)
        {
            fprintf (stderr, "dup2() failed: %s\n", strerror (errno));
            close (fd);
            exit (EXIT_FAILURE);
        }
        if (fd > STDERR_FILENO)
            close (fd);
    }
}

int
main (int argc, char *argv[])
{
    ketopt_t s = KETOPT_INIT;
    int c = 0;
    const char *optstr = "hds:p:";
    int err = -1;

    bool do_daemonize = false;

    while ((c = ketopt (&s, argc, argv, true, optstr, longopts)) != -1)
    {
        switch (c)
        {
        case 'h':
        case ARG_HELP:
            usage (argv[0]);
            return 0;
        case 'd':
        case ARG_DAEMONIZE:
            do_daemonize = true;
            break;
        case 's':
        case ARG_SOCKET_PATH:
            udb_socket_path = s.arg;
            break;
        case 'p':
        case ARG_DB_PATH:
            udb_db_path = s.arg;
            break;
        case '?':
            fprintf (stderr, "Unknown option: %s\n", argv[s.ind]);
            return 1;
        case ':':
            fprintf (stderr, "Option requires an argument: %s\n", argv[s.ind]);
            return 1;
        default:
            UNREACHABLE;
        }
    }

    if (strlen (udb_socket_path) + 1 > sizeof (((struct sockaddr_un *)0)->sun_path))
    {
        fprintf (stderr, "Socket path is too long\n");
        return 1;
    }

    if (do_daemonize)
        daemonize ();

    udb_sockfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
    if (udb_sockfd < 0)
    {
        perror ("socket");
        return 1;
    }

    struct sockaddr_un unsockaddr = { 0 };
    unsockaddr.sun_family = AF_UNIX;
    strncpy (unsockaddr.sun_path, udb_socket_path, sizeof (unsockaddr.sun_path) - 1);

    err = bind (udb_sockfd, (const struct sockaddr *)&unsockaddr, sizeof (unsockaddr));
    if (err < 0)
    {
        if (errno == EADDRINUSE) // socket already exists
        {
            int tfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
            if (tfd < 0)
            {
                perror ("temporary socket");
                close (udb_sockfd);
                return 1;
            }

            if (connect (tfd, (const struct sockaddr *)&unsockaddr, sizeof (unsockaddr)) == 0)
            {
                // Someone is listening
                fprintf (stderr, "Another UDB server is already listening on %s\n",
                         udb_socket_path);
                close (tfd);
                close (udb_sockfd);
                return 1;
            }

            close (tfd);

            if (unlink (udb_socket_path) < 0 && errno != ENOENT)
            {
                perror ("unlink stale socket");
                close (udb_sockfd);
                return 1;
            }

            err = bind (udb_sockfd, (const struct sockaddr *)&unsockaddr, sizeof (unsockaddr));
            if (err < 0)
            {
                perror ("bind after unlink");
                close (udb_sockfd);
                return 1;
            }
        }
        else
        {
            perror ("bind");
            close (udb_sockfd);
            return 1;
        }
    }

    err = listen (udb_sockfd, UDB_SOCKET_BACKLOG);
    if (err < 0)
    {
        perror ("listen");
        close (udb_sockfd);
        return 1;
    }

    if (chmod (udb_socket_path, 0644) < 0)
    {
        perror ("chmod(udb_socket_path)");
        // not fatal
    }

    udb_add_fixed_fd (udb_sockfd, POLLIN);

    if (udb_db_path != NULL)
    {
        err = udb_load_from_file ();
        if (err != 0 && errno != ENOENT)
        {
            fprintf (stderr, "Failed to read database file: %s (%u)\n", strerror (errno), errno);
            close (udb_sockfd);
            return 1;
        }

        int udb_save_timerfd = make_timerfd (5, UDB_DATABASE_SAVE_INTERVAL_SECS);
        if (udb_save_timerfd < 0)
        {
            fprintf (stderr, "Failed to create interval timer: %s (%u)\n", strerror (errno), errno);
            close (udb_sockfd);
            return 1;
        }

        udb_add_fixed_fd (udb_save_timerfd, POLLIN);
    }

    struct sigaction udb_sigaction;
    udb_sigaction.sa_handler = udb_signal_handler;
    sigemptyset (&udb_sigaction.sa_mask);

    sigaction (SIGINT, &udb_sigaction, NULL);
    sigaction (SIGTERM, &udb_sigaction, NULL);

    fprintf (stdout, "listening on %s\n", udb_socket_path);

    while (!udb_quit)
    {
        err = poll (pfds, arrlen (pfds), -1);
        if (err < 0)
        {
            if (errno == EINTR) // handled by udb_signal_handler
                continue;
            perror ("poll");
            break;
        }
        if (err == 0) // timeout
            continue;

        if (pfds[0].revents & POLLIN) // socket
        {
            int cfd = accept (udb_sockfd, NULL, NULL);
            if (cfd < 0)
            {
                if (errno == EINTR) // handled by udb_signal_handler
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                perror ("accept");
                continue;
            }

            ssize_t idx = udb_client_register_fd (cfd);
            if (idx < 0)
            {
                fprintf (stderr, "Failed to register client: %s (%u)\n", strerror (errno), errno);
                close (cfd);
                continue;
            }
        }

        if (udb_db_path && pfds[1].revents & POLLIN) // timer timeout
        {
            unsigned long long expirations;
            ssize_t r = read (pfds[1].fd, &expirations, sizeof (expirations));
            if (r == sizeof (expirations)
                && expirations > 0) // save only once, even if we've missed some expirations
            {
                err = udb_save_to_file ();
                if (err < 0)
                {
                    fprintf (stderr, "Failed to save database to file: %s (%u)\n", strerror (errno),
                             errno);
                    // non-fatal?
                }
            }
            else if (r < 0 && errno != EAGAIN)
            {
                perror ("read(timer_fd)");
            }
        }

        ssize_t total = arrlen (pfds);
        for (ssize_t i = 0; i < total; ++i)
        {
            if (pfds[i].revents == 0)
                continue;
            short ev = pfds[i].revents;

            if (ev & (POLLERR | POLLNVAL))
            {
                fprintf (stderr, "poll reported error/hangup/invalid fd: revents=0x%x\n",
                         pfds[i].revents);
                goto udb_main_exit;
            }
            if (i < (ssize_t)fixed_count)
                continue;

            UDB_ClientContext *c = clients[i];
            if (!c)
                UNREACHABLE;

            if ((ev & POLLHUP) && !(ev & POLLIN))
            {
                // disconnect
                udb_client_unregister_idx (i);
                --i;
                total = arrlen (pfds);
                continue;
            }

            if (ev & POLLIN)
            {
                if (udb_client_read (i) < 0)
                {
                    // disconnect
                    udb_client_unregister_idx (i);
                    --i;
                    total = arrlen (pfds);
                    continue;
                }
            }

            if (ev & POLLOUT)
            {
                if (c->wlen > c->woff)
                {
                    ssize_t w = write (pfds[c->pfd_idx].fd, c->wbuf + c->woff, c->wlen - c->woff);
                    if (w > 0)
                    {
                        c->woff += (size_t)w;
                        if (c->woff == c->wlen)
                        {
                            c->wlen = c->woff = 0;
                            udb_client_pollout_set (i, 0);
                        }
                    }
                    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        udb_client_unregister_idx (i);
                        --i;
                        total = arrlen (pfds);
                        continue;
                    }
                }
            }
        }
    }

udb_main_exit:
    if (pfds)
    {
        for (long i = 0; i < (long)fixed_count; ++i)
        {
            close (pfds[i].fd);

            if (i == 1)
            {
                udb_save_to_file ();
            }
        }

        for (long i = arrlen(pfds) - 1; i >= (long)fixed_count; --i)
        {
            udb_client_unregister_idx(i);
        }

        arrfree (pfds);
        pfds = NULL;
    }
    else
    {
        close (udb_sockfd);
    }

    if (clients)
    {
        for (long i = fixed_count; i < arrlen(clients); ++i)
        {
            UDB_ClientContext* c = clients[i];
            if (c) free(c);
        }

        arrfree (clients);
    }

    udb_sockfd = -1;
    unlink (udb_socket_path);
    return 0;
}

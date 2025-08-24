#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ketopt.h"

#include "config.h"

#define UNREACHABLE                                                                                \
    do                                                                                             \
    {                                                                                              \
        fprintf (stderr, "%s:%u Entered unreachable part of code! Aborting.\n", __FILE__,          \
                 __LINE__);                                                                        \
        abort ();                                                                                  \
    } while (0)

volatile bool udb_quit = false;
int udb_sockfd = -1;

static void
udb_signal_handler (int signo)
{
    (void)signo;
    fprintf (stdout, "\nQuitting...\n");
    udb_quit = true;
}

static char *udb_db_path = NULL;
static char *udb_socket_path = UDB_SOCKET_PATH_DEFAULT;

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
    printf ("Usage: %s [FLAGS]\n", progname);
    printf ("FLAGS:\n");
    printf ("\t-h, --help        - display this message\n");
    printf ("\t-d, --daemonize   - daemonize using double-fork method\n");
    printf ("\t-s, --socket-path - change path to unix socket. DEFAULT: %s\n",
            UDB_SOCKET_PATH_DEFAULT);
    printf ("\t-p, --db-path     - provide path to database file. DEFAULT: NULL (database state "
            "not persisted)\n");
}

int
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
    int c;
    const char *optstr = "hd";

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

    int err;

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
    
    struct sigaction udb_sigaction;
    udb_sigaction.sa_handler = udb_signal_handler;
    sigemptyset (&udb_sigaction.sa_mask);

    sigaction (SIGINT, &udb_sigaction, NULL);
    sigaction (SIGTERM, &udb_sigaction, NULL);

    fprintf (stdout, "listening on %s\n", udb_socket_path);

    struct pollfd pfd = { .fd = udb_sockfd, .events = POLLIN };

    while (!udb_quit)
    {
        int err = poll(&pfd, 1, 500);
        if (err < 0)
        {
            if (errno == EINTR) // handled by udb_signal_handler
                continue;
            perror("poll");
            break;
        }
        if (err == 0) // timeout
            continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "poll reported error/hangup/invalid fd: revents=0x%x\n", pfd.revents);
            break;
        }

        if (pfd.revents & POLLIN)
        {
            int cfd = accept(udb_sockfd, NULL, NULL);
            if (cfd < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                perror("accept");
                continue;
            }

            fprintf(stdout, "client - accepted; pants - shat\n");
            close(cfd);
        }
    }

    close (udb_sockfd);
    udb_sockfd = -1;
    unlink (udb_socket_path);
    return 0;
}

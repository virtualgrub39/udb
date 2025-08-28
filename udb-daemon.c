// requires _POSIX_C_SOURCE=200112L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include "ketopt.h"
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
#define LINELEX_IMPLEMENTATION
#define LINELEX_SHORT_NAMES
#include "linelex.h"

#include "config.h"

#ifndef FD_CLOSURE_CAP
#define FD_CLOSURE_CAP 16384
#endif

typedef struct
{
    ssize_t pfd_idx;
    size_t wlen, woff, aoff;
    char wbuf[UDB_MAX_MSG_LEN];
    char abuf[UDB_MAX_MSG_LEN];
    bool should_exit;
} UDB_ClientContext;

static bool is_daemon = false;
static volatile bool udb_quit = false;

static size_t fixed_count = 0;
static struct pollfd *pfds = NULL;
static UDB_ClientContext **clients = NULL;

enum
{
    T_WHITESPACE = 1,
    T_COMMAND,
    T_ARGUMENT,
};

static Lexer lexer = { 0 };

static int log_level = LOG_NOTICE;
static char *log_filepath = NULL;
static int log_fd = -1;
static const char *log_level_names[] = {
    [LOG_EMERG] = "EMERG",     [LOG_ALERT] = "ALERT",   [LOG_CRIT] = "CRIT", [LOG_ERR] = "ERROR",
    [LOG_WARNING] = "WARNING", [LOG_NOTICE] = "NOTICE", [LOG_INFO] = "INFO", [LOG_DEBUG] = "DEBUG",
};

static inline const char *
safe_strerror (int err, char *buf, size_t buflen)
{
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    char *s = strerror_r (err, buf, buflen);
    return (s ? s : "Unknown error");
#else
    if (strerror_r (err, buf, buflen) == 0)
        return buf;
    snprintf (buf, buflen, "errno %d", err);
    return buf;
#endif
}

static void
logger_init (void)
{
    if (log_filepath)
    {
        log_fd = open (log_filepath, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (log_fd < 0)
        {
            int e = errno;
            fprintf (stderr, "Unable to open log file at %s: %s (%u)", log_filepath, strerror (e),
                     e);
        }
    }

    if (is_daemon && log_fd == -1)
    {
        openlog ("udb", LOG_PID | LOG_CONS, LOG_DAEMON);
    }
}

static void
logger_log (int lvl, const char *fmt, ...)
{
    if (lvl > log_level)
        return;

    char ts_str[64];
    struct timespec ts;

    clock_gettime (CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r (&ts.tv_sec, &tm);
    int ms = ts.tv_nsec / 1000000;

    // ISO 8601-like
    size_t len = strftime ((char *)ts_str, sizeof (ts_str), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf ((char *)ts_str + len, sizeof (ts_str) - len, ".%03d%+03ld%02ld", ms,
              (long)tm.__tm_gmtoff / 3600, labs ((long)tm.__tm_gmtoff) % 3600 / 60);

    va_list ap;
    va_start (ap, fmt);
    char body[UDB_LOGGER_MAX_MSG_LEN];
    vsnprintf (body, sizeof (body), fmt, ap);
    va_end (ap);

    char msg[2 * UDB_LOGGER_MAX_MSG_LEN];

    pid_t pid = getpid ();
    len = snprintf (msg, sizeof (msg), "%s [%s] pid=%d: %s\n", ts_str, log_level_names[lvl],
                    (int)pid, body);

    if (log_fd != -1)
    {
        write (log_fd, msg, len);
    }
    else if (is_daemon)
    {
        syslog (lvl, "%s", body);
    }
    else
    {
        fwrite (msg, 1, len, stderr);
        fflush (stderr);
    }
}

static void
logger_close (void)
{
    if (log_fd != -1)
        close (log_fd);
    log_fd = -1;
    if (is_daemon)
        closelog ();
}

static inline void
logger_log_errno (int level, const char *fmt, ...)
{
    int saved_errno = errno;
    char estr[128];
    const char *estrp = safe_strerror (saved_errno, estr, sizeof (estr));

    char userbuf[1024];
    va_list ap;
    va_start (ap, fmt);
    vsnprintf (userbuf, sizeof (userbuf), fmt, ap);
    va_end (ap);

    logger_log (level, "%s : %s (%d)", userbuf, estrp, saved_errno);
}

#define UNREACHABLE                                                                                \
    do                                                                                             \
    {                                                                                              \
        logger_log (LOG_CRIT, "%s:%u Entered unreachable part of code! Aborting.", __FILE__,       \
                    __LINE__);                                                                     \
        abort ();                                                                                  \
    } while (0)

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

    int flags = fcntl (cfd, F_GETFL);
    if (flags == -1)
        return -1;
    if (fcntl (cfd, F_SETFL, flags | O_NONBLOCK) == -1)
        return -1;

    int fdflags = fcntl (cfd, F_GETFD);
    if (fdflags == -1)
        return -1;
    if (fcntl (cfd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
        return -1;

    struct pollfd p = { .fd = cfd, .events = POLLIN, .revents = 0 };

    arrput (pfds, p);
    arrput (clients, c);

    ssize_t idx = (ssize_t)arrlen (pfds) - 1;
    c->pfd_idx = idx;

    logger_log (LOG_DEBUG, "Registered client idx=%zd (fd=%d)", idx, cfd);

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

    shutdown (pfds[idx].fd, SHUT_RDWR);
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

    logger_log (LOG_DEBUG, "Unregistered idx=%zu", idx);

    return 0;
}

enum
{
    UDB_PFX_NONE,
    UDB_PFX_OK,
    UDB_PFX_ERR,
};

const char *UDB_PFX_STRINGS[] = {
    [UDB_PFX_NONE] = "",
    [UDB_PFX_OK] = "OK",
    [UDB_PFX_ERR] = "ERR",
};

static ssize_t
udb_client_write_async (int idx, int pfx, const char *msg)
{
    if (!msg || strlen (msg) > UDB_MAX_MSG_LEN - 4)
    {
        errno = EINVAL;
        return -1;
    }

    UDB_ClientContext *c = clients[idx];
    if (!c)
        UNREACHABLE;

    if (c->wlen != 0)
    {
        errno = EAGAIN;
        return -1;
    }

    int len = snprintf (c->wbuf, sizeof c->wbuf, "%s%s%s\r\n", UDB_PFX_STRINGS[pfx],
                        (pfx != UDB_PFX_NONE && msg[0]) ? " " : "", msg);
    if (len < 0 || (size_t)len >= sizeof c->wbuf)
    {
        errno = E2BIG;
        return -1;
    }
    c->wlen = (size_t)len;
    c->woff = 0;

    udb_client_pollout_set (idx, 1);

    return 0;
}

static const char *
udb_handle_get (TokenArray args)
{
    (void)args;
    errno = ENODATA;
    return NULL;
}

static ssize_t
udb_handle_set (TokenArray args)
{
    (void)args;
    errno = EINVAL;
    return -1;
}

static const char *
udb_handle_del (TokenArray args)
{
    (void)args;
    errno = ENODATA;
    return NULL;
}

static ssize_t
udb_client_read (size_t idx)
{
    UDB_ClientContext *c = clients[idx];
    if (!c)
        UNREACHABLE;

    int cfd = pfds[c->pfd_idx].fd;

    bool terminator_received = false;

    while (true)
    {
        ssize_t n = read (cfd, c->abuf + c->aoff, UDB_MAX_MSG_LEN - c->aoff);
        if (n <= 0)
            return n;

        c->aoff += n;

        if (strstr (c->abuf, "\r\n") != NULL)
        {
            terminator_received = true;
            break;
        }

        if (c->aoff >= UDB_MAX_MSG_LEN)
            break;
    }

    if (!terminator_received)
    {
        if (c->aoff != UDB_MAX_MSG_LEN)
            return 0;

        int err = udb_client_write_async (idx, UDB_PFX_ERR, "message to long");
        if (err < 0)
            return err;
        c->should_exit = true;
        return 0;
    }

    logger_log (LOG_DEBUG, "client[%lu]: %.*s", idx, (int)c->aoff - 2, c->abuf);

    TokenArray ta = NULL;
    ssize_t result = lexer_lex (&lexer, c->abuf, &ta);
    if (result < 0 || ta[0].type != T_COMMAND)
    {
        result = udb_client_write_async (idx, UDB_PFX_ERR, "invalid command");
        c->should_exit = true;
        return result;
    }

    const char *cmd_ptr = ta[0].ptr;
    size_t cmd_len = ta[0].len;
    arrdel (ta, 0);

    if (strncmp (cmd_ptr, "GET", cmd_len) == 0)
    {
        const char *v = udb_handle_get (ta);
        if (v == NULL)
        {
            switch (errno)
            {
            case ENODATA:
                result = udb_client_write_async (idx, UDB_PFX_ERR, "");
                break;
            default:
                logger_log_errno (LOG_WARNING, "DB GET failed");
                result = udb_client_write_async (idx, UDB_PFX_ERR, "internal server error");
                break;
            }
        }

        result = udb_client_write_async (idx, UDB_PFX_OK, v);
    }
    else if (strncmp (cmd_ptr, "SET", cmd_len) == 0)
    {
        result = udb_handle_set (ta);
        if (result < 0)
        {
            logger_log_errno (LOG_WARNING, "DB SET failed");
            result = udb_client_write_async (idx, UDB_PFX_ERR, "internal server error");
        }

        result = udb_client_write_async (idx, UDB_PFX_OK, "");
    }
    else if (strncmp (cmd_ptr, "DEL", cmd_len) == 0)
    {
        const char *v = udb_handle_del (ta);
        if (v == NULL)
        {
            switch (errno)
            {
            case ENODATA:
                result = udb_client_write_async (idx, UDB_PFX_OK, "");
                break;
            default:
                logger_log_errno (LOG_WARNING, "DB DEL failed");
                result = udb_client_write_async (idx, UDB_PFX_ERR, "internal server error");
                break;
            }
        }

        result = udb_client_write_async (idx, UDB_PFX_OK, v);
    }
    else
        UNREACHABLE;

    arrfree (ta);
    return result;
}

static void
udb_signal_handler (int signo)
{
    logger_log (LOG_NOTICE, "Signal [%d] received. Flipping quit flag.", signo);
    udb_quit = true;
}

static int
write_pidfile (const char *path)
{
    mode_t oldmask = umask (0);
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    umask (oldmask);

    if (fd < 0)
    {
        logger_log_errno (LOG_ERR, "Pidfile creation failed");
        return -1;
    }
    char buf[32];
    int len = snprintf (buf, sizeof buf, "%d\n", (int)getpid ());
    if (write (fd, buf, len) != len)
    {
        logger_log_errno (LOG_ERR, "Failed to write to pidfile");
        close (fd);
        return -1;
    }
    fsync (fd);
    close (fd);

    logger_log (LOG_DEBUG, "Pidfile written successfully");

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

static char *udb_db_path = UDB_DATABASE_FILE_PATH_DEFAULT;
static char *udb_socket_path = UDB_SOCKET_PATH_DEFAULT;

static int
udb_load_from_file (void)
{
    logger_log (LOG_DEBUG, "udb_load_from_file() called");
    return 0;
}

static int
udb_save_to_file (void)
{
    logger_log (LOG_DEBUG, "udb_save_to_file() called");
    return 0;
}

enum
{
    ARG_HELP = 256,
    ARG_DAEMONIZE,
    ARG_SOCKET_PATH,
    ARG_DB_PATH,
    ARG_LOGFILE,
    ARG_LOGLVL,
};

static const ko_longopt_t longopts[] = {
    { "help", ko_no_argument, ARG_HELP },
    { "daemonize", ko_no_argument, ARG_DAEMONIZE },
    { "socket-path", ko_required_argument, ARG_SOCKET_PATH },
    { "db-path", ko_required_argument, ARG_DB_PATH },
    { "log-file", ko_required_argument, ARG_LOGFILE },
    { "log-level", ko_required_argument, ARG_LOGLVL },
    { NULL, 0, 0 },
};

static void
usage (const char *progname)
{
    printf ("Usage: %s [FLAGS] [ARGS]\n", progname);
    printf ("FLAGS:\n");
    printf ("\t-h, --help        - display this message\n");
    printf ("\t-D, --daemonize   - daemonize using double-fork method\n");
    printf ("ARGS:\n");
    printf ("\t-s, --socket-path - change path to unix socket. DEFAULT: %s\n",
            UDB_SOCKET_PATH_DEFAULT);
    printf ("\t-p, --db-path     - provide path to database file. DEFAULT: %s\n",
            UDB_DATABASE_FILE_PATH_DEFAULT ? UDB_DATABASE_FILE_PATH_DEFAULT
                                           : "NULL (not persisted)");
    printf ("\t-v, --log-level   - verbosity 0..7. DEFAULT: 5\n");
    printf ("\t--log-file        - set path to logfile\n");
}

static void
daemonize ()
{
    logger_log (LOG_DEBUG, "Begun to daemonize");

    pid_t pid;

    pid = fork ();
    if (pid < 0)
    {
        logger_log_errno (LOG_CRIT, "First fork() failed");
        exit (EXIT_FAILURE);
    }
    if (pid > 0)
    {
        _exit (EXIT_SUCCESS);
    }

    if (setsid () < 0)
    {
        logger_log_errno (LOG_CRIT, "setsid() failed");
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
        logger_log_errno (LOG_CRIT, "Second fork() failed");
        exit (EXIT_FAILURE);
    }
    if (pid > 0)
    {
        _exit (EXIT_SUCCESS);
    }

    logger_log (LOG_DEBUG, "Second fork() done");

    write_pidfile ("/tmp/udb.pid");

    umask (0);
    if (chdir ("/") < 0)
    {
        logger_log_errno (LOG_ERR, "chdir(\"/\") failed");
    }

    { // close all fds (except the fixed and log_fd)
        struct rlimit rl;
        rlim_t maxfd = 0;
        if (getrlimit (RLIMIT_NOFILE, &rl) == 0)
        {
            if (rl.rlim_max == RLIM_INFINITY)
                maxfd = FD_CLOSURE_CAP;
            else if (rl.rlim_max > FD_CLOSURE_CAP)
                maxfd = FD_CLOSURE_CAP;
            else
                maxfd = rl.rlim_max;
        }
        else
        {
            long m = sysconf (_SC_OPEN_MAX);
            if (m < 0)
                maxfd = 1024;
            else if ((rlim_t)m > FD_CLOSURE_CAP)
                maxfd = FD_CLOSURE_CAP;
            else
                maxfd = (rlim_t)m;
        }

        for (int fd = 0; fd < (int)maxfd; ++fd)
        {
            int keepthis = 0;
            for (int i = 0; i < arrlen (pfds); ++i) // no clients were accepted just yet
            {
                if (pfds[i].fd == fd)
                {
                    keepthis = 1;
                    break;
                }
            }
            if (fd == log_fd)
                keepthis = true;

            if (!keepthis)
                close (fd);
        }
    }

    { // redirect stdout/stderr/stdin
        int fd = open ("/dev/null", O_RDWR);
        if (fd < 0)
        {
            logger_log_errno (LOG_CRIT, "open(\"/dev/null\") failed");
            exit (EXIT_FAILURE);
        }
        if (dup2 (fd, STDIN_FILENO) < 0 || dup2 (fd, STDOUT_FILENO) < 0
            || dup2 (fd, STDERR_FILENO) < 0)
        {
            logger_log_errno (LOG_CRIT, "dup2() failed");
            close (fd);
            exit (EXIT_FAILURE);
        }
        if (fd > STDERR_FILENO)
            close (fd);
    }

    logger_log (LOG_DEBUG, "Successfully daemonized");

    is_daemon = true;
}

int
main (int argc, char *argv[])
{
    ketopt_t s = KETOPT_INIT;
    int c = 0;
    const char *optstr = "hDs:p:v:";
    ssize_t err = -1;

    logger_init ();

    while ((c = ketopt (&s, argc, argv, true, optstr, longopts)) != -1)
    {
        switch (c)
        {
        case 'h':
        case ARG_HELP:
            usage (argv[0]);
            return 0;
        case 'D':
        case ARG_DAEMONIZE:
            is_daemon = true;
            break;
        case 's':
        case ARG_SOCKET_PATH:
            udb_socket_path = s.arg;
            break;
        case 'p':
        case ARG_DB_PATH:
            udb_db_path = s.arg;
            break;
        case 'v':
        case ARG_LOGLVL:
            log_level = atoi (s.arg);
            if (log_level < 0 || log_level > 7)
            {
                logger_log (LOG_ERR, "Invalid log level");
                return 1;
            }
            break;
        case ARG_LOGFILE:
            log_filepath = s.arg;
            break;
        case '?':
            logger_log (LOG_ERR, "Unknown option: %s", argv[s.ind - 1]);
            return 1;
        case ':':
            logger_log (LOG_ERR, "Option requires an argument: %s", argv[s.ind - 1]);
            return 1;
        default:
            UNREACHABLE;
        }
    }

    logger_init ();

    if (strlen (udb_socket_path) + 1 > sizeof (((struct sockaddr_un *)0)->sun_path))
    {
        logger_log (LOG_ERR, "Socket path is too long");
        return 1;
    }

    // logger_log(LOG_DEBUG, "debug :p");
    // logger_log(LOG_INFO, "info :3");
    // logger_log(LOG_NOTICE, "notice :*");
    // logger_log(LOG_WARNING, "warning :/");
    // logger_log(LOG_ERR, "error :(");
    // logger_log(LOG_CRIT, "critical :'(");
    // logger_log(LOG_ALERT, "alert :o");
    // logger_log(LOG_EMERG, "EMERGENCY");

    // logger_close();

    // return 0;

    const TokenType tokdefs[] = {
        TokenTypeDef (T_WHITESPACE, "WHITESPACE", "[\t\r\n ]+", true),
        TokenTypeDef (T_COMMAND, "COMMAND", "(GET)|(SET)|(DEL)", false),
        TokenTypeDef (T_ARGUMENT, "ARG_NUM_F32", "[0-9]*\\.[0-9]+([eE][+-]?[0-9]+)?", false),
        TokenTypeDef (T_ARGUMENT, "ARG_NUM_I", "[0-9]+", false),
        TokenTypeDef (T_ARGUMENT, "ARG_STR_Q", "\"([^\"\\\\]|\\\\.)*\"", false),
        TokenTypeDef (T_ARGUMENT, "ARG_STR", "[a-zA-Z_][a-zA-Z0-9_]*", false),
    };

    lexer_init (&lexer, .auto_anchor = true, .case_insensitive = false, .use_extended_regex = true);
    long defcount = sizeof (tokdefs) / sizeof (tokdefs[0]);
    for (long i = 0; i < defcount; ++i)
    {
        err = token_type_adds (&lexer, tokdefs[i]);
        if (err != 0)
        {
            logger_log_errno (LOG_CRIT, "Failed to add token type %s (%zu)", tokdefs[i].name, i);
            LL_lexer_cleanup (&lexer);
            return 1;
        }
    }

    int udb_sockfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
    if (udb_sockfd < 0)
    {
        logger_log_errno (LOG_CRIT, "Socket creation failed");
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
            logger_log (LOG_INFO, "%s already exists. Checking for staleness...", udb_socket_path);

            int tfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
            if (tfd < 0)
            {
                logger_log_errno (LOG_CRIT, "Temporary socket creation failed");
                close (udb_sockfd);
                return 1;
            }

            if (connect (tfd, (const struct sockaddr *)&unsockaddr, sizeof (unsockaddr)) == 0)
            {
                // Someone is listening
                logger_log (LOG_CRIT, "Another UDB server is already listening on %s",
                            udb_socket_path);
                close (tfd);
                close (udb_sockfd);
                return 1;
            }

            close (tfd);

            if (unlink (udb_socket_path) < 0 && errno != ENOENT)
            {
                logger_log_errno (LOG_CRIT, "Failed to unlink stale socket");
                close (udb_sockfd);
                return 1;
            }

            err = bind (udb_sockfd, (const struct sockaddr *)&unsockaddr, sizeof (unsockaddr));
            if (err < 0)
            {
                logger_log_errno (LOG_CRIT, "Failed to bind after closing stale socket");
                close (udb_sockfd);
                return 1;
            }
        }
        else
        {
            logger_log_errno (LOG_CRIT, "Failed to bind to %s", udb_socket_path);
            close (udb_sockfd);
            return 1;
        }
    }

    err = listen (udb_sockfd, UDB_SOCKET_BACKLOG);
    if (err < 0)
    {
        logger_log_errno (LOG_CRIT, "listen()");
        close (udb_sockfd);
        return 1;
    }

    int flags = fcntl (udb_sockfd, F_GETFL, 0);
    if (flags < 0)
    {
        logger_log_errno (LOG_CRIT, "fcntl(F_GETFL) for main socket");
        close (udb_sockfd);
        return 1;
    }
    err = fcntl (udb_sockfd, F_SETFL, flags | O_NONBLOCK);
    if (err < 0)
    {
        logger_log_errno (LOG_CRIT, "fcntl(F_SETFL) for main socket");
        close (udb_sockfd);
        return 1;
    }

    flags = fcntl (udb_sockfd, F_GETFD);
    if (flags == -1)
    {
        close (udb_sockfd);
        return 1;
    }
    if (fcntl (udb_sockfd, F_SETFD, flags | FD_CLOEXEC) == -1)
    {
        close (udb_sockfd);
        return 1;
    }

    if (chmod (udb_socket_path, 0644) < 0)
    {
        logger_log_errno (LOG_ERR, "chmod(udb_socket_path)");
        // not fatal
    }

    udb_add_fixed_fd (udb_sockfd, POLLIN);

    if (udb_db_path != NULL)
    {
        err = udb_load_from_file ();
        if (err != 0 && errno != ENOENT)
        {
            logger_log_errno (LOG_CRIT, "Failed to read database file");
            close (udb_sockfd);
            return 1;
        }

        if (UDB_DATABASE_SAVE_INTERVAL_SECS != 0)
        {
            int udb_save_timerfd = make_timerfd (5, UDB_DATABASE_SAVE_INTERVAL_SECS);
            if (udb_save_timerfd < 0)
            {
                logger_log_errno (LOG_CRIT, "Failed to create interval timer");
                close (udb_sockfd);
                return 1;
            }

            udb_add_fixed_fd (udb_save_timerfd, POLLIN);
        }
    }

    struct sigaction udb_sigaction;
    udb_sigaction.sa_handler = udb_signal_handler;
    sigemptyset (&udb_sigaction.sa_mask);

    sigaction (SIGINT, &udb_sigaction, NULL);
    sigaction (SIGTERM, &udb_sigaction, NULL);

    logger_log (LOG_NOTICE, "Listening on %s", udb_socket_path);

    if (is_daemon)
        daemonize ();

    while (!udb_quit)
    {
        err = poll (pfds, arrlen (pfds), -1);
        logger_log (LOG_DEBUG, "poll() unblocked");
        if (err < 0)
        {
            if (errno == EINTR) // handled by udb_signal_handler
                continue;
            logger_log_errno (LOG_CRIT, "poll()");
            break;
        }
        if (err == 0) // timeout
            continue;

        if (pfds[0].revents & POLLIN) // socket
        {
            int cfd = accept (udb_sockfd, NULL, NULL);

            do
            {
                if (cfd < 0)
                    break;

                ssize_t idx = udb_client_register_fd (cfd);
                if (idx < 0)
                {
                    logger_log_errno (LOG_WARNING, "Failed to register client");
                    close (cfd);
                    continue;
                }

                cfd = accept (udb_sockfd, NULL, NULL);
            } while (cfd > 0);

            if (errno == EINTR) // handled by udb_signal_handler
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            logger_log_errno (LOG_CRIT, "accept");
            continue;
        }

        if (udb_db_path && UDB_DATABASE_SAVE_INTERVAL_SECS
            && pfds[1].revents & POLLIN) // timer timeout
        {
            unsigned long long expirations;
            ssize_t r = read (pfds[1].fd, &expirations, sizeof (expirations));
            if (r < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                else
                    // logger_log_errno (LOG_CRIT, "read(timer_fd)");
                    logger_log_errno (LOG_ERR, "Database auto-save timerfd read() failed");
            }

            if (expirations > 0)
            {
                err = udb_save_to_file ();
                if (err < 0)
                {
                    logger_log_errno (LOG_ERR, "Failed to save database to file");
                }
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
                logger_log (LOG_CRIT, "`poll` reported error/hangup/invalid fd: revents=0x%x",
                            pfds[i].revents);
                if (i < (ssize_t)fixed_count)
                {
                    logger_log (LOG_CRIT, "fixed fd #%zd failed: exiting", i);
                    goto udb_main_exit;
                }
                else
                {
                    udb_client_unregister_idx (i);
                    --i;
                    total = arrlen (pfds);
                    continue;
                }
            }
            if (i < (ssize_t)fixed_count) // fixed fds already handled
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
                logger_log (LOG_DEBUG, "POLLIN on idx=%zd", i);
                err = udb_client_read (i);

                if (err < 0)
                {
                    if (errno == EAGAIN || errno == EINTR)
                        continue;

                    // disconnect
                    udb_client_unregister_idx (i);
                    --i;
                    total = arrlen (pfds);
                    continue;
                }

                if (err == 0)
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
                logger_log (LOG_DEBUG, "POLLOUT on idx=%zd", i);

                if (c->wlen > c->woff)
                {
                    ssize_t w = write (pfds[i].fd, c->wbuf + c->woff, c->wlen - c->woff);
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

                if (c->should_exit)
                {
                    udb_client_unregister_idx (i);
                    --i;
                    total = arrlen (pfds);
                    continue;
                }
            }
        }
    }

    logger_log (LOG_DEBUG, "Main loop break");

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

        for (long i = arrlen (pfds) - 1; i >= (long)fixed_count; --i)
        {
            udb_client_unregister_idx (i);
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
        for (long i = fixed_count; i < arrlen (clients); ++i)
        {
            UDB_ClientContext *c = clients[i];
            if (c)
                free (c);
        }

        arrfree (clients);
    }

    lexer_cleanup (&lexer);

    logger_log (LOG_DEBUG, "Deinit done - about to close");
    logger_close ();

    udb_sockfd = -1;
    unlink (udb_socket_path);
    return 0;
}

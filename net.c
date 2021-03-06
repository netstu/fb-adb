#include <sys/un.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/wait.h>
#include "util.h"
#include "net.h"
#include "child.h"
#include "fs.h"
#include "fdrecorder.h"

#if defined(__linux__) && !defined(SOCK_CLOEXEC)
# define SOCK_CLOEXEC O_CLOEXEC
#endif

#if !defined(SUN_LEN) && defined(__linux__)
# define SUN_LEN(ptr) ((size_t) (((struct sockaddr_un *) 0)->sun_path)  \
                       + strlen ((ptr)->sun_path))
#endif

char*
describe_addr(const struct addr* a)
{
    sa_family_t family = a->addr.sa_family;
    char* ret;
    switch (family) {
        case AF_INET: {
            char buf[INET_ADDRSTRLEN];
            if (!inet_ntop(family, &a->addr_in.sin_addr, buf, sizeof (buf)))
                die_errno("inet_ntop");
            ret = xaprintf("[%s:%hu]", buf, ntohs(a->addr_in.sin_port));
            break;
        }
        case AF_INET6: {
            char buf[INET6_ADDRSTRLEN];
            if (!inet_ntop(family, &a->addr_in6.sin6_addr, buf, sizeof (buf)))
                die_errno("inet_ntop");
            ret = xaprintf("[%s:%hu]", buf, ntohs(a->addr_in6.sin6_port));
            break;
        }
        case AF_UNIX: {
            const char* kind;
            const char* path = &a->addr_un.sun_path[0];
            size_t path_offset = offsetof(struct addr, addr_un.sun_path);
            if (path_offset > a->size)
                die(EINVAL, "illegal AF_UNIX addr");
            size_t pathlen = a->size - path_offset;
            if (pathlen > INT_MAX)
                pathlen = INT_MAX;
            if (path[0] == '\0') {
                kind = "abstract";
                path++;
            } else {
                kind = "filesystem";
            }
            ret = xaprintf("[unix%s:[%.*s]]", kind, (int) pathlen, path);
            break;
        }
        default:
            ret = xaprintf("[unknown address family %d]", (int) family);
    }
    return ret;
}

struct addr*
make_addr_unix_filesystem(const char* filename)
{
    size_t filename_length = strlen(filename);
    size_t addrlen = offsetof(struct addr, addr_un.sun_path);
    if (SATADD(&addrlen, addrlen, filename_length + 1))
        die(EINVAL, "socket name too long");

    struct addr* a = xalloc(addrlen);
    memset(a, 0, offsetof(struct addr, addr_un.sun_path));
    a->addr_un.sun_family = AF_UNIX;
    memcpy(a->addr_un.sun_path, filename, filename_length + 1);
    a->size = SUN_LEN(&a->addr_un);
    return a;
}

struct addr*
make_addr_unix_abstract(const void* bytes, size_t nr)
{
#ifdef __linux__
    size_t addrlen = offsetof(struct addr, addr_un.sun_path) + 1;
    if (SATADD(&addrlen, addrlen, nr))
        die(EINVAL, "socket name too long");

    struct addr* a = xalloc(addrlen);
    a->size = addrlen - offsetof(struct addr, addr_un);
    a->addr_un.sun_family = AF_UNIX;
    a->addr_un.sun_path[0] = '\0';
    memcpy(a->addr_un.sun_path+1, bytes, nr);
    return a;
#else
    die(ENOSYS, "this system does not support abstract AF_UNIX");
#endif
}

struct addr*
make_addr_unix_abstract_s(const char* name)
{
    return make_addr_unix_abstract(name, strlen(name));
}

void
xconnect(int fd, const struct addr* addr)
{
    int rc;

    do {
        WITH_IO_SIGNALS_ALLOWED();
        rc = connect(fd, &addr->addr, addr->size);
    } while (rc == -1 && errno == EINTR);

    if (rc == -1)
        die_errno("connect");
}

void
xlisten(int fd, int backlog)
{
    if (listen(fd, backlog) == -1)
        die_errno("listen");
}

void
xbind(int fd, const struct addr* addr)
{
    if (bind(fd, &addr->addr, addr->size) == -1)
        die_errno("bind");
}

static void
xgetaddrinfo_cleanup(void* data)
{
    freeaddrinfo((struct addrinfo*) data);
}

struct addrinfo*
xgetaddrinfo(const char* node,
             const char* service,
             const struct addrinfo* hints)
{
    int rc;
    struct cleanup* cl = cleanup_allocate();
    struct addrinfo* res = NULL;

    do {
        rc = getaddrinfo(node, service, hints, &res);
    } while (rc == EAI_SYSTEM && errno == EINTR);

    if (rc == EAI_SYSTEM)
        die_errno("getaddrinfo");

    if (rc != 0)
        die(ENOENT, "getaddrinfo failed: %s", gai_strerror(rc));

    cleanup_commit(cl, xgetaddrinfo_cleanup, res);
    return res;
}

struct addr*
addrinfo2addr(const struct addrinfo* ai)
{
    size_t allocsz = offsetof(struct addr, addr);
    if (SATADD(&allocsz, allocsz, ai->ai_addrlen))
        die(EINVAL, "address too long");

    struct addr* a = xalloc(allocsz);
    a->size = ai->ai_addrlen;
    memcpy(&a->addr, ai->ai_addr, ai->ai_addrlen);
    return a;
}

void
xsetsockopt(int fd, int level, int opname,
            void* optval, socklen_t optlen)
{
    if (setsockopt(fd, level, opname, optval, optlen) == -1)
        die_errno("setsockopt");
}

int
xsocket(int domain, int type, int protocol)
{
    struct cleanup* cl = cleanup_allocate();

#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    int s = socket(domain, type, protocol);
    if (s < 0)
        die_errno("socket");

    cleanup_commit_close_fd(cl, s);

#ifndef SOCK_CLOEXEC
    merge_O_CLOEXEC_into_fd_flags(s, O_CLOEXEC);
#endif

    assert_cloexec(s);
    return s;
}

int
xaccept_internal(int server_socket, bool allow_eagain)
{
    struct cleanup* cl = cleanup_allocate();
    int s;

    do {
        WITH_IO_SIGNALS_ALLOWED();
#ifdef HAVE_ACCEPT4
        s = accept4(server_socket, NULL, NULL, SOCK_CLOEXEC);
#else
        s = accept(server_socket, NULL, NULL);
#endif
    } while (s == -1 && errno == EINTR);

    if (allow_eagain && s == -1 &&
        (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        return -1;
    }

    if (s == -1)
        die_errno("accept");

    cleanup_commit_close_fd(cl, s);

#ifndef HAVE_ACCEPT4
    merge_O_CLOEXEC_into_fd_flags(s, O_CLOEXEC);
#endif

    assert_cloexec(s);
    return s;
}

int
xaccept(int server_socket)
{
    return xaccept_internal(server_socket, false);
}

int
xaccept_nonblock(int server_socket)
{
    int socket = xaccept_internal(server_socket, true);
    if (socket != -1) {
#ifndef __linux__ // Linux guarantees O_NONBLOCK is not inherited
        fd_set_blocking_mode(socket, blocking);
#endif
    }
    return socket;
}

void
xsocketpair(int domain, int type, int protocol,
            int* s1, int* s2)
{
    struct cleanup* cl[2];
    cl[0] = cleanup_allocate();
    cl[1] = cleanup_allocate();

#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    int fd[2];
    if (socketpair(domain, type, protocol, fd) < 0)
        die_errno("socketpair");

    cleanup_commit_close_fd(cl[0], fd[0]);
    cleanup_commit_close_fd(cl[1], fd[1]);

#ifndef SOCK_CLOEXEC
    merge_O_CLOEXEC_into_fd_flags(fd[0], O_CLOEXEC);
    merge_O_CLOEXEC_into_fd_flags(fd[1], O_CLOEXEC);
#endif

    assert_cloexec(fd[0]);
    assert_cloexec(fd[1]);

    *s1 = fd[0];
    *s2 = fd[1];
}

void
xsocketpairnc(int domain, int type, int protocol, int sv[2])
{
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    if (socketpair(domain, type, protocol, sv) < 0)
        die_errno("socketpair");

#ifndef SOCK_CLOEXEC
    merge_O_CLOEXEC_into_fd_flags(sv[0], O_CLOEXEC);
    merge_O_CLOEXEC_into_fd_flags(sv[1], O_CLOEXEC);
#endif

    assert_cloexec(sv[0]);
    assert_cloexec(sv[1]);
}

void
disable_tcp_nagle(int fd)
{
    int on = 1;
    xsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

struct write_all_or_die {
    int fd;
    const void* buf;
    size_t sz;
};

static void
write_all_or_die_1(void* data)
{
    struct write_all_or_die* waod = data;
    write_all(waod->fd, waod->buf, waod->sz);
}

static void
write_all_or_die(int fd, const void* buf, size_t sz)
{
    struct write_all_or_die waod = {
        .fd = fd,
        .buf = buf,
        .sz = sz,
    };

    if (catch_error(write_all_or_die_1, &waod, NULL))
        abort();
}

struct xgai {
    const char* node;
    const char* service;
    const struct addrinfo* hints;
};

static void
write_blob(int fd, const void* data, size_t sz)
{
    write_all(fd, &sz, sizeof (sz));
    write_all(fd, data, sz);
}

static void
xgai_preexec_1(void* data)
{
    struct xgai* xa = data;
    int fd = STDOUT_FILENO;
    for (struct addrinfo* ai =
             xgetaddrinfo(xa->node, xa->service, xa->hints);
         ai;
         ai = ai->ai_next)
    {
        write_blob(fd, ai, sizeof (*ai));
        write_blob(fd, ai->ai_addr, ai->ai_addrlen);
        if (ai->ai_canonname)
            write_blob(fd, ai->ai_canonname, strlen(ai->ai_canonname)+1);
    }
}

static void
xgai_preexec(void* data)
{
    sigset_t no_signals;
    sigemptyset(&no_signals);
    memcpy(&signals_unblock_for_io, &no_signals, sizeof (sigset_t));

    struct errinfo ei = { .want_msg = true };
    if (catch_error(xgai_preexec_1, data, &ei)) {
        write_all_or_die(2, ei.msg, strlen(ei.msg));
        _exit(1);
    }

    _exit(0);
}

static void*
decode_blob(uint8_t** data_inout, size_t* sz_out, uint8_t* data_end)
{
    uint8_t* data = *data_inout;
    size_t sz;
    if (data_end - data < sizeof (sz))
        die(ECOMM, "truncated data");

    memcpy(&sz, data, sizeof (sz));
    data += sizeof (sz);

    if (data_end - data < sz)
        die(ECOMM, "truncated data");

    void* blob = data;
    data += sz;

    *data_inout = data;
    *sz_out = sz;
    return blob;
}

struct addrinfo*
xgetaddrinfo_interruptible(
    const char* node,
    const char* service,
    const struct addrinfo* hints)
{
    struct xgai xa = {
        .node = node,
        .service = service,
        .hints = hints,
    };

    struct child_start_info csi = {
        .io[STDIN_FILENO] = CHILD_IO_DEV_NULL,
        .io[STDOUT_FILENO] = CHILD_IO_PIPE,
        .io[STDERR_FILENO] = CHILD_IO_RECORD,
        .pre_exec = xgai_preexec,
        .pre_exec_data = &xa,
    };

    struct child* child = child_start(&csi);
    struct growable_buffer out =
        slurp_fd_buf(child->fd[STDOUT_FILENO]->fd);
    int status = child_wait(child);
    bool success = child_status_success_p(status);
    if (!success) {
        if (WIFEXITED(status)) {
            char* err = massage_output_buf(
                fdrecorder_get_clean(
                    child->recorder[STDERR_FILENO]));
            die(ENOENT, "%s", err);
        } else if (WIFSIGNALED(status)) {
            die(ENOENT,
                "getaddrinfo failed with signal %d",
                WTERMSIG(status));
        } else {
            die(ENOENT, "unknown status from resolver process");
        }
    }

    // Subprocesses supposedly succeeded.  Read from the serialized
    // GAI information.

    uint8_t* data = (uint8_t*) out.buf;
    uint8_t* data_end = data + out.bufsz;
    struct addrinfo* ai_list = NULL;
    struct addrinfo** next = &ai_list;

    while (data < data_end) {
        struct addrinfo* ai;
        size_t sz;

        ai = decode_blob(&data, &sz, data_end);
        if (sz != sizeof (*ai))
            die(ECOMM, "gai protocol error");

        ai->ai_addr = decode_blob(&data, &sz, data_end);
        if (sz != ai->ai_addrlen)
            die(ECOMM, "gai protocol error");

        if (ai->ai_canonname)
            ai->ai_canonname = decode_blob(&data, &sz, data_end);

        *next = ai;
        next = &ai->ai_next;
    }

    return ai_list;
}

void
xshutdown(int socketfd, int how)
{
    if (shutdown(socketfd, how) == -1)
        die_errno("shutdown(%d, %d)", socketfd, how);
}

#ifdef SO_PEERCRED
struct ucred
get_peer_credentials(int socketfd)
{
    struct ucred peer_credentials;
    socklen_t optlen = sizeof (peer_credentials);
    memset(&peer_credentials, 0, sizeof (peer_credentials));
    if (getsockopt(socketfd, SOL_SOCKET, SO_PEERCRED,
                   &peer_credentials, &optlen)
        == -1)
    {
        die_errno("getsockopt");
    }

    if (optlen != sizeof (peer_credentials))
        die(ECOMM, "bad length frm SO_PEERCRED");

    return peer_credentials;
}
#endif

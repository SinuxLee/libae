#ifndef SOCKET_COMPAT_H
#define SOCKET_COMPAT_H

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#undef FD_SETSIZE
#define FD_SETSIZE 655350

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stddef.h>

#define POLLRDNORM  0x0100
#define POLLRDBAND  0x0200
#define POLLIN      (POLLRDNORM | POLLRDBAND)
#define POLLPRI     0x0400

#define POLLWRNORM  0x0010
#define POLLOUT     (POLLWRNORM)
#define POLLWRBAND  0x0020

#define POLLERR     0x0001
#define POLLHUP     0x0002
#define POLLNVAL    0x0004

typedef unsigned long       nfds_t;
typedef signed long         ssize_t;
typedef int                 socklen_t;
typedef int                 pid_t;
#ifndef mode_t
#define mode_t            unsigned __int32
#endif

// fcntl flags used in Redis
#define	F_GETFL		3
#define	F_SETFL		4
#define	O_NONBLOCK	0x0004

typedef struct {
    SOCKET  fd;
    short   events;
    short   revents;

}pollfd;


int win32_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
const char *win32_gai_strerror(int errcode);
void win32_freeaddrinfo(struct addrinfo *res);
SOCKET win32_socket(int domain, int type, int protocol);
int win32_ioctl(SOCKET fd, unsigned long request, unsigned long *argp);
int win32_bind(SOCKET sockfd, const struct sockaddr *addr, socklen_t addrlen);
int win32_connect(SOCKET sockfd, const struct sockaddr *addr, socklen_t addrlen);
int win32_getsockopt(SOCKET sockfd, int level, int optname, void *optval, socklen_t *optlen);
int win32_setsockopt(SOCKET sockfd, int level, int optname, const void *optval, socklen_t optlen);
int win32_close(SOCKET fd);
ssize_t win32_recv(SOCKET sockfd, void *buf, size_t len, int flags);
ssize_t win32_send(SOCKET sockfd, const void *buf, size_t len, int flags);
int win32_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int win32_fcntl(SOCKET sockfd, int cmd, int flags);

#ifndef SOCKCOMPAT_IMPLEMENTATION
#define getaddrinfo(node, service, hints, res) win32_getaddrinfo(node, service, hints, res)
#undef gai_strerror
#define gai_strerror(errcode) win32_gai_strerror(errcode)
#define freeaddrinfo(res) win32_freeaddrinfo(res)
#define socket(domain, type, protocol) win32_socket(domain, type, protocol)
#define ioctl(fd, request, argp) win32_ioctl(fd, request, argp)
#define bind(sockfd, addr, addrlen) win32_bind(sockfd, addr, addrlen)
#define connect(sockfd, addr, addrlen) win32_connect(sockfd, addr, addrlen)
#define getsockopt(sockfd, level, optname, optval, optlen) win32_getsockopt(sockfd, level, optname, optval, optlen)
#define setsockopt(sockfd, level, optname, optval, optlen) win32_setsockopt(sockfd, level, optname, optval, optlen)
#define close(fd) win32_close(fd)
#define read(sockfd, buf, len) win32_recv(sockfd, buf, len, 0)
#define write(sockfd, buf, len) win32_send(sockfd, buf, len, 0)
#define recv(sockfd, buf, len, flags) win32_recv(sockfd, buf, len, flags)
#define send(sockfd, buf, len, flags) win32_send(sockfd, buf, len, flags)
#define poll(fds, nfds, timeout) win32_poll(fds, nfds, timeout)
#define fcntl(sockfd, cmd, flags) win32_fcntl(sockfd, cmd, flags)
#endif

#endif
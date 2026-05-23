#include "tun.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#ifdef __linux__

#include <linux/if.h>
#include <linux/if_tun.h>

int tun_open(char *name_buf, size_t name_buf_len) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    /* IFF_TUN: layer-3 packets (no Ethernet header).
     * IFF_NO_PI: do not prepend the 4-byte struct tun_pi to each packet. */
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (name_buf && name_buf[0])
        strncpy(ifr.ifr_name, name_buf, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (name_buf && name_buf_len > 0) {
        strncpy(name_buf, ifr.ifr_name, name_buf_len - 1);
        name_buf[name_buf_len - 1] = '\0';
    }
    return fd;
}

ssize_t tun_read(int fd, void *buf, size_t buf_len) {
    return read(fd, buf, buf_len);
}

ssize_t tun_write(int fd, const void *buf, size_t buf_len) {
    return write(fd, buf, buf_len);
}

#elif defined(__APPLE__)

#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/uio.h>

int tun_open(char *name_buf, size_t name_buf_len) {
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) return -1;

    struct ctl_info ci;
    memset(&ci, 0, sizeof(ci));
    strncpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof(ci.ctl_name) - 1);
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof(sc));
    sc.sc_id = ci.ctl_id;
    sc.sc_len = sizeof(sc);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_unit = 0; /* 0 means "kernel, pick a free utunN for me" */

    if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (name_buf && name_buf_len > 0) {
        socklen_t name_len = (socklen_t)name_buf_len;
        if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME,
                       name_buf, &name_len) < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    return fd;
}

/* macOS utun prepends a 4-byte protocol family identifier (network byte
 * order) to every packet on both read and write. Strip it on read and
 * inject it on write so the rest of the stack only deals with raw IP. */
ssize_t tun_read(int fd, void *buf, size_t buf_len) {
    uint32_t pf;
    struct iovec iov[2] = {
        { .iov_base = &pf,  .iov_len = sizeof(pf) },
        { .iov_base = buf,  .iov_len = buf_len    },
    };
    ssize_t n = readv(fd, iov, 2);
    if (n < (ssize_t)sizeof(pf)) return -1;
    return n - (ssize_t)sizeof(pf);
}

ssize_t tun_write(int fd, const void *buf, size_t buf_len) {
    uint32_t pf = htonl(AF_INET);
    struct iovec iov[2] = {
        { .iov_base = &pf,         .iov_len = sizeof(pf) },
        { .iov_base = (void *)buf, .iov_len = buf_len    },
    };
    ssize_t n = writev(fd, iov, 2);
    if (n < (ssize_t)sizeof(pf)) return -1;
    return n - (ssize_t)sizeof(pf);
}

#else
#error "usertcp: unsupported platform — only Linux and macOS are supported"
#endif

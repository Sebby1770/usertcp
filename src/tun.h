#ifndef USERTCP_TUN_H
#define USERTCP_TUN_H

#include <stddef.h>
#include <sys/types.h>

/* Open the OS-specific TUN device.
 *
 * Linux:  if name_buf is non-empty, attach to that pre-created persistent
 *         device (typically created via `ip tuntap add ... user $USER`).
 *         If empty, the kernel picks a name. IFF_NO_PI is set so no
 *         packet-info prefix is added to reads/writes.
 * macOS:  opens a utun control socket; the kernel picks a free utunN.
 *         The chosen name is written back into name_buf.
 *
 * Returns the fd on success, -1 on failure (errno set). */
int tun_open(char *name_buf, size_t name_buf_len);

/* Read one IP packet from the TUN. On macOS the 4-byte protocol-family
 * prefix is stripped so callers always see a pure IP packet. */
ssize_t tun_read(int fd, void *buf, size_t buf_len);

/* Write one IP packet to the TUN. On macOS the 4-byte protocol-family
 * header is prepended automatically (AF_INET for IPv4). */
ssize_t tun_write(int fd, const void *buf, size_t buf_len);

#endif

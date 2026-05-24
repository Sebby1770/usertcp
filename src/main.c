#include "tun.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

static void hexdump(const unsigned char *data, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        printf("  %04zx  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) printf("%02x ", data[i + j]);
            else            printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; ++j) {
            unsigned char c = data[i + j];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }
}

int main(int argc, char **argv) {
    char ifname[IFNAMSIZ] = {0};
    if (argc > 1) strncpy(ifname, argv[1], sizeof(ifname) - 1);

    int fd = tun_open(ifname, sizeof(ifname));
    if (fd < 0) {
        fprintf(stderr, "tun_open: %s\n", strerror(errno));
#if defined(__APPLE__)
        if (errno == EPERM)
            fprintf(stderr,
                    "hint: macOS utun requires root. try: sudo ./usertcp\n");
#else
        if (errno == EPERM || errno == EACCES)
            fprintf(stderr,
                    "hint: run scripts/setup-linux.sh first to create a "
                    "persistent tun owned by you.\n");
#endif
        return 1;
    }

    fprintf(stderr, "opened tun device: %s (fd=%d)\n", ifname, fd);
#if defined(__APPLE__)
    fprintf(stderr,
            "\n  in another terminal, bring the interface up:\n"
            "    sudo ifconfig %s 10.0.0.1 10.0.0.2 up\n"
            "    sudo route -nq add -net 10.0.0.0/24 -interface %s\n"
            "\n  then send some traffic:\n"
            "    ping 10.0.0.2\n"
            "\n  press Ctrl-C to quit.\n\n",
            ifname, ifname);
#else
    fprintf(stderr,
            "\n  if you haven't already, run scripts/setup-linux.sh.\n"
            "  then in another terminal: ping 10.0.0.2\n"
            "\n  press Ctrl-C to quit.\n\n");
#endif

    unsigned char buf[2048];
    unsigned long packet_no = 0;
    for (;;) {
        ssize_t n = tun_read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "tun_read: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) continue;

        printf("packet #%lu  len=%zd\n", ++packet_no, n);
        hexdump(buf, (size_t)n);
        printf("\n");
        fflush(stdout);
    }
}

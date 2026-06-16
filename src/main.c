#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "stack.h"
#include "tun.h"

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

/* The address the stack answers to. The host side of the TUN is 10.0.0.1;
 * this stack is its peer at 10.0.0.2. */
#define STACK_IP "10.0.0.2"

static int g_tun_fd = -1;

/* Output sink: write one IP packet to the TUN device. */
static void tun_output(void *user, const uint8_t *pkt, size_t len) {
    (void)user;
    if (tun_write(g_tun_fd, pkt, len) < 0)
        fprintf(stderr, "tun_write: %s\n", strerror(errno));
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

int main(int argc, char **argv) {
    char ifname[IFNAMSIZ] = {0};
    if (argc > 1)
        strncpy(ifname, argv[1], sizeof(ifname) - 1);

    g_tun_fd = tun_open(ifname, sizeof(ifname));
    if (g_tun_fd < 0) {
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

    struct stack stack;
    stack_init(&stack, inet_addr(STACK_IP), tun_output, NULL);
    apps_register(&stack);

    fprintf(stderr, "usertcp up on %s as %s (fd=%d)\n", ifname, STACK_IP,
            g_tun_fd);
#if defined(__APPLE__)
    fprintf(stderr,
            "\n  in another terminal, bring the interface up:\n"
            "    sudo ifconfig %s 10.0.0.1 %s up\n"
            "    sudo route -nq add -net 10.0.0.0/24 -interface %s\n",
            ifname, STACK_IP, ifname);
#else
    fprintf(stderr, "\n  if you haven't already, run scripts/setup-linux.sh.\n");
#endif
    fprintf(stderr,
            "\n  then try:\n"
            "    ping %s                 # ICMP echo\n"
            "    nc -u %s 7              # UDP echo (type, see it bounce)\n"
            "    nc %s 9999              # TCP echo\n"
            "    curl http://%s/         # HTTP/1.0 served by the stack\n"
            "\n  Ctrl-C to quit.\n\n",
            STACK_IP, STACK_IP, STACK_IP, STACK_IP);

    unsigned char buf[2048];
    struct pollfd pfd = {.fd = g_tun_fd, .events = POLLIN};

    for (;;) {
        int pr = poll(&pfd, 1, 50 /* ms: bound timer latency */);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll: %s\n", strerror(errno));
            break;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = tun_read(g_tun_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "tun_read: %s\n", strerror(errno));
                break;
            }
            if (n > 0)
                stack_input(&stack, buf, (size_t)n);
        }

        stack_tick(&stack, now_ms());
    }

    close(g_tun_fd);
    return 0;
}

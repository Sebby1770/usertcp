# usertcp

A user-space TCP/IP stack written from scratch in C, on top of a TUN
device. A learning project: the goal is to understand what every kernel
TCP implementation does, by writing one.

**Status:** Milestone 0 — TUN device read loop. The program opens a TUN
on Linux or macOS and hex-dumps every IP packet that arrives. No parsing
yet; that starts at milestone 1.

The full project roadmap (12 milestones, ending in HTTPS served from the
custom stack) is in [docs/roadmap.md](docs/roadmap.md).

## Building

```sh
make
```

Only standard libc + POSIX. No external dependencies.

## Running

### Linux

```sh
./scripts/setup-linux.sh   # one-time: create tun0, assign 10.0.0.1/24
./usertcp tun0             # in one shell

# in another shell:
ping 10.0.0.2
```

You'll see hex dumps of the ICMP echo request packets. There's nothing
listening on 10.0.0.2 yet, so `ping` will just time out — that's
expected; milestone 1 is when it starts replying.

### macOS

```sh
./usertcp                                       # in one shell;
                                                # note the utunN it prints
./scripts/setup-macos.sh utunN 10.0.0.1 10.0.0.2  # in another shell
ping 10.0.0.2
```

macOS assigns the utun number when the program connects, so the bring-up
script has to run *after* the program starts.

## What you'll see

Each incoming packet prints as:

```
packet #1  len=84
  0000  45 00 00 54 00 00 40 00  40 01 a8 a3 0a 00 00 01  |E..T..@.@.......|
  0010  0a 00 00 02 08 00 ...
```

The leading `45` is your first IPv4 header — `4` is the version, `5` is
the header length in 32-bit words (5 × 4 = 20 bytes). Already enough to
start parsing if you wanted to peek ahead.

## Layout

```
.
├── src/
│   ├── main.c   # entry point: open TUN, read loop, hexdump
│   ├── tun.h    # TUN open/read/write interface
│   └── tun.c    # Linux + macOS implementations
├── scripts/
│   ├── setup-linux.sh
│   └── setup-macos.sh
├── docs/
│   └── roadmap.md
├── Makefile
├── LICENSE      # MIT
└── README.md
```

## Roadmap (high level)

0. **TUN read loop** ← current
1. IPv4 parsing + ICMP echo (so `ping` works)
2. UDP echo
3. TCP three-way handshake
4. TCP data transfer (stop-and-wait + RTO)
5. TCP teardown (FIN dance, TIME_WAIT)
6. Sliding window + flow control
7. Congestion control (Reno)
8. Multi-connection demux + sockets-like API
9. HTTP/1.0 server over the stack
10. TLS integration → HTTPS
11. Observability: pcap-out, structured tracing, introspection

Each milestone has its own scope, expected tests, and debugging
workflow. See [docs/roadmap.md](docs/roadmap.md) for the full breakdown.

## Debugging toolkit

- `tcpdump -i tun0 -nn -X -e` — see the kernel's view of the same packets
- Wireshark on `tun0` — interactive packet inspection, "Follow TCP Stream"
- `tc qdisc add dev tun0 root netem loss 5% delay 50ms` (Linux) — inject
  loss and latency to test reliability and congestion control
- `ss -tinp` — Linux's view of any kernel-side TCP connections

## References

- TCP/IP Illustrated, Vol. 1 (Stevens / Fall) — implementation companion
- RFC 9293 — TCP (the modern consolidated spec)
- RFC 6298 — RTO computation
- RFC 5681 — TCP congestion control (Reno)
- RFC 8446 — TLS 1.3

## License

MIT. See [LICENSE](LICENSE).

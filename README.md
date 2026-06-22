# usertcp

A user-space TCP/IP stack written from scratch in C, on top of a TUN
device. A learning project: the goal is to understand what every kernel
TCP implementation does, by writing one.

**Status:** the stack answers `ping`, echoes UDP, completes the full TCP
lifecycle (3-way handshake → reliable data transfer → teardown with
TIME_WAIT), and serves HTTP/1.0 — all framed by hand from the IP header up.
Everything below the TUN glue is unit-tested on the host without root.

```
ping 10.0.0.2          # ICMP echo reply
nc -u 10.0.0.2 7       # UDP echo
nc 10.0.0.2 9999       # TCP echo
curl http://10.0.0.2/  # HTTP/1.0 served straight off the stack
```

The full project roadmap (12 milestones, ending in HTTPS) is in
[docs/roadmap.md](docs/roadmap.md).

## What's implemented

| Layer | Done |
|---|---|
| **IPv4** | header parse + validation, checksum verify/generate, addressed-to-us filtering, fragment drop |
| **ICMP** | echo request → echo reply (`ping` works) |
| **UDP** | demux by port, pseudo-header checksum, echo server on port 7 |
| **TCP** | passive open (SYN/SYN-ACK/ACK), in-order reliable data, sliding send window honoring the peer's advertised window, **Reno congestion control** (slow start, congestion avoidance, fast retransmit + fast recovery), Go-Back-N retransmission, RFC 6298 RTO with Karn's algorithm, full close (FIN_WAIT_1/2, CLOSING, TIME_WAIT, CLOSE_WAIT, LAST_ACK), RST generation for unmatched segments, multi-connection demux by 4-tuple |
| **Apps** | TCP echo, HTTP/1.0 `GET` server |

Not yet (see roadmap): SACK, window scaling, out-of-order reassembly,
active open / sockets API, IPv6, TLS.

## Architecture

The protocol core is deliberately transport-agnostic. `stack_input()`
consumes one inbound IPv4 packet; an output callback emits one outbound
IPv4 packet. `main.c` wires those to a TUN device; the test harness wires
them to in-memory buffers — so the entire stack is exercisable without a
TUN or root.

```
            ┌───────────────┐
 TUN fd ───▶│  stack_input  │──▶ IPv4 ─┬─▶ ICMP ─▶ echo reply
            └───────────────┘          ├─▶ UDP  ─▶ port table ─▶ app
            ┌───────────────┐          └─▶ TCP  ─▶ 4-tuple TCB ─▶ app
 TUN fd ◀───│ output(pkt)   │◀───────────────────────────────────┘
            └───────────────┘
```

| File | Responsibility |
|---|---|
| `src/net.{h,c}` | wire-format structs, internet + pseudo-header checksums, 32-bit serial-number arithmetic |
| `src/stack.{h,c}` | IPv4 parse/dispatch, ICMP echo, UDP, the shared IP-send path |
| `src/tcp.{h,c}` | the TCP state machine, send/receive paths, RTO timers |
| `src/app.{h,c}` | demo apps: UDP echo, TCP echo, HTTP/1.0 |
| `src/tun.{h,c}` | Linux + macOS TUN open/read/write |
| `src/main.c` | event loop: `poll()` the TUN, feed the stack, tick timers |
| `tests/test_stack.c` | host-side protocol tests (no root) |

## Building

```sh
make        # build ./usertcp
make test   # build and run the host-side protocol tests (no root needed)
```

Only standard libc + POSIX. No external dependencies.

## Running

### Linux

```sh
./scripts/setup-linux.sh   # one-time: create tun0, assign 10.0.0.1/24
./usertcp tun0             # in one shell
```

### macOS

The utun control socket requires root:

```sh
sudo ./usertcp
```

It prints the `utunN` name the kernel assigned plus the exact
`ifconfig`/`route` commands to bring it up — copy them into another shell.

### Then, from any shell

```sh
ping 10.0.0.2             # replies now, instead of timing out
nc -u 10.0.0.2 7          # type a line, watch it bounce
nc 10.0.0.2 9999          # TCP echo
curl -v http://10.0.0.2/  # a page served by the stack itself
```

## Testing

`make test` runs `tests/test_stack.c`, which redirects the stack's output
into an in-memory capture queue, pushes crafted packets through
`stack_input()`, and checks every emitted packet field by field — the same
idea as `packetdrill`, scoped down to one self-contained C file. It covers:

- the internet checksum (incl. the RFC 1071 worked example) and 32-bit
  sequence arithmetic across the wraparound point
- ICMP echo (address swap, checksum, id/seq echoed)
- UDP echo (pseudo-header checksum, payload round-trip)
- TCP: RST to a closed port, then the full SYN → SYN-ACK → ACK → data echo
  → FIN lifecycle with sequence-number bookkeeping verified at each step
- a corrupt IP checksum being dropped

Because the core has no I/O dependencies, these run in milliseconds and
need no privileges — ideal for CI.

## Debugging toolkit

- `tcpdump -i tun0 -nn -X -e` — see the kernel's view of the same packets
- Wireshark on `tun0` — interactive inspection, "Follow TCP Stream"
- `tc qdisc add dev tun0 root netem loss 5% delay 50ms` (Linux) — inject
  loss and latency to watch the RTO and retransmission paths fire
- `ss -tinp` — Linux's view of any kernel-side TCP connections

## References

- TCP/IP Illustrated, Vol. 1 (Stevens / Fall) — implementation companion
- RFC 9293 — TCP (the modern consolidated spec)
- RFC 1071 — computing the internet checksum
- RFC 6298 — RTO computation
- RFC 5681 — TCP congestion control (Reno) — the next milestone
- RFC 8446 — TLS 1.3

## License

MIT. See [LICENSE](LICENSE).

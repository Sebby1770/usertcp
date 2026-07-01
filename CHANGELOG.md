# Changelog

All notable changes to usertcp are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- **TCP out-of-order reassembly** (milestone 6 receive side, `src/tcp.c`):
  segments that arrive ahead of `rcv_nxt` but within the receive window are
  parked in a small per-connection table and delivered in order once the gap
  is filled (instead of being dropped). 6 new host-test checks.
- **TCP Reno congestion control** (milestone 7, `src/tcp.c`): slow start
  (IW=10·MSS, RFC 6928), congestion avoidance, fast retransmit + fast
  recovery on three duplicate ACKs, and an RTO that collapses `cwnd` to one
  MSS. The sender is now limited by `min(cwnd, rwnd)`. 11 new host-test
  checks drive the full cwnd lifecycle.
- **IPv4** layer: header parse + validation, checksum verify and generate,
  addressed-to-us filtering, fragment drop. (`src/stack.c`, `src/net.{c,h}`)
- **ICMP** echo: replies to `ping 10.0.0.2` (milestone 1).
- **UDP**: port demux, pseudo-header checksum, echo server on port 7
  (milestone 2).
- **TCP** (`src/tcp.{c,h}`): passive open / three-way handshake, reliable
  in-order data transfer over a sliding send window that honours the peer's
  advertised window, Go-Back-N retransmission, RFC 6298 RTO estimation with
  Karn's algorithm, the full close sequence (FIN_WAIT_1/2, CLOSING,
  TIME_WAIT, CLOSE_WAIT, LAST_ACK), RST generation for unmatched segments,
  and multi-connection demux by 4-tuple (milestones 3–5, 8).
- **Apps** (`src/app.{c,h}`): TCP echo server (port 9999) and a minimal
  HTTP/1.0 `GET` server (port 80) (milestone 9).
- **Host-side test harness** (`tests/test_stack.c`): a packetdrill-style
  suite that drives crafted packets through the transport-agnostic core and
  checks every emitted packet — 31 checks, no TUN and no root required.
  `make test`.
- `poll()`-based event loop in `main.c` with a timer tick for retransmission
  and TIME_WAIT timers.

### Changed
- Reworked the stack around a transport-agnostic core (`stack_input` +
  output callback) so the protocol logic is unit-testable without a TUN
  device. `main.c` is no longer a hexdumper.
- `README.md` and `docs/roadmap.md` updated to reflect milestones 1–5/8/9.

### Fixed
- Stopped tracking stale build artifacts; `.gitignore` now also covers
  `*.dSYM/` and the test binary.

## [milestone-0] - 2026-05-24

### Added
- TUN device open/read/write on Linux and macOS; a read loop that hexdumps
  every inbound IP packet.

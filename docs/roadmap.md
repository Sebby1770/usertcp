# usertcp roadmap

A 3–6 month project to build a working user-space TCP/IP stack in C,
ending in HTTPS served over the custom stack. The goal is depth of
understanding, not features — each milestone introduces one concept and
adds the tests + debugging workflow that proves it works.

## Upfront decisions

- **Language:** C. Matches RFC pseudocode 1:1; no abstractions hiding
  what the bytes look like.
- **Wire interface:** TUN device. Layer-3 only; no Ethernet or ARP to
  worry about. (Bonus stretch later: TAP + ARP.)
- **OS:** Linux for full ecosystem (tc/netem, packetdrill, ss);
  macOS utun supported for native dev on Apple Silicon.
- **TLS:** integrate a library (mbedtls or BoringSSL). Hand-rolling
  TLS 1.3 is a bigger project than the rest of this combined.

## Key trade-offs and choices

| Decision | Choices | Pick | Why |
|---|---|---|---|
| Wire interface | raw socket / TUN | TUN | Isolated from kernel stack; no ARP races |
| Stack location | kernel / user-space | user-space | Full visibility, debugger-friendly |
| TCP state machine | full / minimal | full | Implement properly, simplify TIME_WAIT only |
| Reliability | stop-and-wait / sliding window | both, in sequence | Learn RTO first, then upgrade |
| Congestion control | none / Reno / CUBIC | Reno | Captures every important idea |
| TLS | hand-rolled / library | library | TLS is its own months-long project |

---

## Milestone 0 — Toolchain & "hello packets"

**Goal.** Open a TUN device, assign it an IP, print every IP packet
that arrives.

**Concepts.** TUN/TAP virtual interfaces; kernel routing to a TUN by
destination IP; reading from a TUN fd; network byte order.

**Steps.**
1. `ip tuntap add dev tun0 mode tun user $USER` (Linux); `utun` control
   socket connect (macOS).
2. `ip addr add 10.0.0.1/24 dev tun0 && ip link set tun0 up` (Linux);
   `ifconfig utunN 10.0.0.1 10.0.0.2 up` (macOS).
3. In your program: open the fd, `read()` in a loop, hex-dump.
4. From another terminal: `ping 10.0.0.2`. Watch the hex.

**Edge cases.** Permissions; MTU; endianness; macOS 4-byte protocol
family prefix on every packet.

**Tests.** Hex dump matches Wireshark's view of the same traffic.

**Debugging.** `tcpdump -i tun0 -nn -X -e`; Wireshark on `tun0`.

**Stretch.** Cross-platform parity (this milestone already does both).

---

## Milestone 1 — IPv4 parsing + ICMP echo

**Goal.** Reply to `ping 10.0.0.2`.

**Concepts.** IPv4 header layout; total length vs IHL; TTL;
fragmentation (understand, ignore); the IP checksum (one's-complement
of one's-complement sum — practice this); ICMP as an L4 peer.

**Headers.** IPv4 (20+ bytes); ICMP echo request/reply (Type 8 / 0 +
id + sequence + payload).

**Steps.**
1. Parse IPv4 — version, IHL, total length, protocol, src/dst, checksum.
2. Validate IP checksum on input.
3. If protocol == 1 (ICMP) and type == 8: swap src/dst, type → 0,
   recompute ICMP checksum (header + payload), recompute IP checksum
   (header only).
4. Write the packet back to the TUN fd.

**Edge cases.** IP options (IHL > 5); byte order on every field;
fragment offset != 0.

**Tests.** `ping -c 5 10.0.0.2` at low RTT. `ping -s 1400 10.0.0.2`.
Corrupt outgoing checksum and confirm Linux drops it.

**Debugging.** `tcpdump -i tun0 icmp -X`; Wireshark "Bad checksum"
highlight.

**Stretch.** ICMP timestamp; basic fragmentation reassembly; respond
to traceroute with ICMP Time Exceeded for low-TTL packets.

---

## Milestone 2 — UDP echo

**Goal.** Bind port 7 and bounce UDP datagrams back.

**Concepts.** UDP as connectionless L4; **pseudo-header checksum** (the
gotcha — UDP/TCP checksums cover a synthesized header with src/dst IP
and protocol); demultiplexing by destination port.

**Headers.** UDP: src port, dst port, length, checksum.

**Steps.**
1. Branch on IP protocol: 17 → UDP.
2. Verify UDP checksum with the IPv4 pseudo-header.
3. `(port → callback)` table for app listeners.
4. Echo callback: swap src/dst at both IP and UDP layers.

**Edge cases.** Zero-length payloads; max-size datagrams; checksum 0
vs 0xFFFF.

**Tests.** `nc -u 10.0.0.2 7`; `iperf3 -u -c 10.0.0.2 -p 7` for load.

**Debugging.** `tcpdump -i tun0 udp -X`.

**Stretch.** Tiny DNS responder for a hardcoded zone.

---

## Milestone 3 — TCP three-way handshake

**Goal.** Accept SYN, send SYN-ACK, accept final ACK, sit in
ESTABLISHED. No data transfer yet.

**Concepts.** TCP as a state machine; ISN — pick randomly per RFC 6528;
SEQ means "first byte in this segment", ACK means "next byte I expect";
the **+1 quirk** (SYN consumes a sequence number); MSS option.

**State.** CLOSED → LISTEN → SYN_RCVD → ESTABLISHED.

**Steps.**
1. Parse TCP header. Validate TCP checksum (pseudo-header).
2. TCB hash table keyed by 4-tuple.
3. SYN to listening port → pick ISN, SYN+ACK with
   `ack = their_seq + 1` → SYN_RCVD.
4. Final ACK → validate `ack == our_isn + 1` → ESTABLISHED.
5. Negotiate MSS.

**Edge cases.** Simultaneous open (defer). SYN to non-listening port →
RST. Duplicate SYN → resend SYN-ACK. Bad seq on final ACK → drop.

**Tests.** `nc 10.0.0.2 9999` establishes. Wireshark confirms seq math.

**Debugging.** Wireshark "Follow TCP Stream"; `ss -tnp`.

**Stretch.** SYN cookies (RFC 4987); window scaling + timestamps.

---

## Milestone 4 — TCP data transfer (stop-and-wait)

**Goal.** Receive, ACK, send, retransmit on timeout. App can
read/write a byte stream.

**Concepts.** Sequence space (32-bit modular — write `seq_lt(a, b)`);
cumulative ACKs; **RTO** per RFC 6298 (SRTT, RTTVAR, exponential
backoff); receive buffer.

**Steps.**
1. Data segment + `seq == rcv_nxt` → append, advance, ACK.
2. Out-of-order → drop, dup ACK.
3. App `write()` → one MSS segment, RTO timer, wait for ACK. Throughput
   will be awful — fixed in milestone 6.
4. RTO fires → resend, double timer.

**Edge cases.** Sequence wraparound; ACK for un-sent data (drop); ACK
older than `snd_una` (ignore); RST → CLOSED.

**Tests.** `nc 10.0.0.2 9999` echoes. `iperf3` (record the awful
number). `tc qdisc add dev tun0 root netem loss 5%` → retransmits.

**Debugging.** Wireshark → Statistics → TCP Stream Graphs →
Time/Sequence.

**Stretch.** Nagle; delayed ACK; observe the Nagle+delayed-ACK pessimization.

---

## Milestone 5 — TCP teardown

**Goal.** Clean four-way close. Survive TIME_WAIT.

**Concepts.** FIN consumes a seq number; half-close; TIME_WAIT (catch
delayed duplicates + give your last ACK retransmission cover).

**State.** Add FIN_WAIT_1, FIN_WAIT_2, CLOSING, TIME_WAIT, CLOSE_WAIT,
LAST_ACK, CLOSED.

**Steps.**
1. `close()` + buffer drained → FIN → FIN_WAIT_1.
2. ACK of our FIN → FIN_WAIT_2.
3. Their FIN → ACK → TIME_WAIT, start 2·MSL timer.
4. Passive close: their FIN → CLOSE_WAIT; app close → LAST_ACK;
   their ACK → CLOSED.

**Edge cases.** Simultaneous close → CLOSING. FIN with data
piggy-backed. Data after our FIN.

**Tests.** Normal close; rapid-fire 100 open/close cycles (no TCB
leaks).

**Debugging.** `dump-connections` debug command; Wireshark color-codes
FIN.

**Stretch.** RFC 5961 challenge ACKs.

---

## Milestone 6 — Sliding window + flow control

**Goal.** Multiple segments in flight. Honor peer's advertised window.

**Concepts.** Window = bytes in flight; `snd_wnd` from incoming ACKs;
receiver advertises `rcv_wnd` = free recv buffer; zero-window probing;
silly window syndrome.

**Steps.**
1. Track `snd_una`, `snd_nxt`, `snd_wnd`. Send up to
   `snd_una + snd_wnd`.
2. Each ACK advances `snd_una`, updates `snd_wnd`.
3. Advertise `rcv_wnd` on every outgoing segment.
4. Zero-window → 1-byte probe.

**Edge cases.** Window updates without ACK advancing; silly window —
don't advertise tiny increases.

**Tests.** `iperf3` throughput jumps 10–100×. With `netem delay 50ms`,
watch BDP-limited rate.

**Debugging.** Wireshark TCP Stream Graph → Window Scaling;
`ss -tinm`.

**Stretch.** Window scaling option (essential ≥1ms RTT); SACK (RFC 2018).

---

## Milestone 7 — Congestion control (Reno)

**Goal.** Don't melt the network. Slow start, congestion avoidance,
fast retransmit, fast recovery.

**Concepts.** AIMD; `cwnd` vs `rwnd` — sender uses `min(cwnd, rwnd)`;
`ssthresh`; dup-ACK as a loss signal; RTO as a worse loss signal.

**Steps.**
1. Init `cwnd = 10·MSS` (RFC 6928), `ssthresh = 64 KB` or `∞`.
2. Slow start: each ACK → `cwnd += MSS`. Exit at `cwnd ≥ ssthresh`.
3. Congestion avoidance: each ACK → `cwnd += MSS·MSS/cwnd`.
4. 3 dup ACKs → fast retransmit + fast recovery:
   `ssthresh = cwnd/2`, `cwnd = ssthresh + 3·MSS`.
5. RTO → `ssthresh = cwnd/2`, `cwnd = 1·MSS`, back to slow start.

**Edge cases.** Don't inflate past `rwnd`; **Karn's algorithm** (don't
sample RTT from retransmits); Reno's limits under multi-loss windows.

**Tests.** `tc qdisc add dev tun0 root netem loss 1% delay 50ms`.
Throughput stabilizes near `MSS / (RTT · √loss)` (Mathis formula).

**Debugging.** Plot `cwnd` over time — the sawtooth pattern is the
moment it clicks.

**Stretch.** CUBIC; pacing; BBR (much bigger lift).

---

## Milestone 8 — Multi-connection + sockets API

**Goal.** N simultaneous connections. Expose
`socket/bind/listen/accept/read/write/close`.

**Concepts.** Demux by 4-tuple; listen backlog (SYN + accept queues);
event loop vs threads; epoll/kqueue analog.

**Steps.**
1. Hash table: 4-tuple → TCB.
2. Listeners keyed by `(local_ip, local_port)`.
3. SYN → SYN queue; final ACK → accept queue.
4. App `accept()` → next from accept queue.

**Edge cases.** Backlog overflow → drop or RST; duplicate SYN; TIME_WAIT
4-tuple collision (RFC 6191).

**Tests.** `ab -n 1000 -c 50` load. `dump-connections` returns to
baseline.

**Debugging.** Per-connection log levels; "show me one connection"
command.

**Stretch.** Readiness API (epoll-shape); Unix-domain control socket.

---

## Milestone 9 — HTTP/1.0 server

**Goal.** Serve a static page over the stack.

**Steps.** Read until `\r\n\r\n`. Parse method/path/version.
`GET /` → `200 OK` + Content-Length + body. Close.

**Edge cases.** Slow-loris → header-read timeout; bad request → 400.

**Tests.** `curl -v http://10.0.0.2:9999/`. Malformed requests.

**Stretch.** HTTP/1.1 keep-alive; chunked; range requests.

---

## Milestone 10 — TLS → HTTPS

**Goal.** Serve `https://10.0.0.2/`.

**Concepts.** TLS as byte-stream-over-byte-stream; the TLS library has
its own state machine, you feed it network bytes and it gives you
plaintext + bytes to send.

**Steps.**
1. Self-signed cert:
   `openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 30 -nodes`.
2. Integrate mbedtls (or BoringSSL). Wire the BIO to your TCP
   read/write.
3. Wrap `accept()` with `accept_tls()` that runs the handshake first.

**Edge cases.** TLS `close_notify` before TCP FIN; cert expiry.

**Tests.** `curl -k https://10.0.0.2/`; `openssl s_client`.

**Debugging.** `SSLKEYLOGFILE=keys.log curl -k ...` — load into
Wireshark to decrypt the capture.

**Stretch.** Hand-rolled TLS 1.3 (months of work).

---

## Milestone 11 — Observability

**Goal.** Make the stack debuggable by future-you.

**Steps.**
1. Emit pcap dumps of every packet sent/received.
2. Structured per-connection tracing (state transitions + seq + window
   + cwnd + ssthresh).
3. `/_admin/connections` endpoint with live TCB state.
4. Counters: bytes in/out, retransmits, dup ACKs, RTO hits.
   Histograms: RTT, segment size.

**Stretch.** Built-in REPL; Perfetto/Chrome-trace integration for
packet-flow flame graphs.

---

## Tools you'll lean on throughout

- **Wireshark / `tshark`** — primary truth source; learn
  `tcp.analysis.retransmission`, `tcp.flags.syn == 1`.
- **`tcpdump`** — headless captures: `tcpdump -i tun0 -w out.pcap`.
- **`ip`, `ss`, `ethtool`** — Linux's view.
- **`tc` + `netem`** — inject loss, delay, reordering. Critical from
  milestone 4 onward.
- **`netcat` / `socat`** — quick clients/servers.
- **`iperf3`** — throughput benchmarks.
- **`hping3`** — pathological packets (bad checksums, weird flags).
- **`packetdrill`** — script-driven TCP behavior tests.

## Essential reading (in order of usefulness)

- TCP/IP Illustrated, Vol. 1 (Stevens / Fall) — read *alongside*
  implementation, not before.
- RFC 9293 — modern consolidated TCP spec (supersedes RFC 793 +
  patches).
- RFC 791 — IPv4.
- RFC 6298 — RTO computation.
- RFC 5681 — TCP congestion control (Reno).
- RFC 6928 — IW=10.
- RFC 2018 — SACK.
- RFC 6191 — TIME-WAIT and quick reuse.
- RFC 8446 — TLS 1.3 (read for understanding).
- `smoltcp` source (Rust) — read for design ideas, not to copy.

# Multiplayer (paused, help wanted)

Off by default. Enable with `POPTB_NET=1` (loads the recompiled `WEANETR.dll`). Without it, stubs in
`runtime/misc.c` report "no network" and the game runs single player normally.

## What works
- `WEANETR.dll` is recompiled together with the game (`gen/`, `gen_uw/`).
- `runtime/winsock.c`: Winsock on BSD sockets (TCP, UDP, `select`, `ioctlsocket`, name resolution).
- Per-thread message queues (`runtime/user32.c`): weanetr runs its own message loop.
- Multiplayer menu lists "Winsock TCP/IP Internet Connection" and "Head-to-head modem".
- Hosting: the host opens UDP 7575 and reaches the "Connected" lobby screen.
- LAN discovery: a second instance (same Mac or LAN) sees the session (`POP 1/4 1.01`) and offers JOIN GAME.

## Windows/macOS differences already handled
1. `gethostbyname(<machine name>)` may hang on mDNS → fixed local name `poptb-mac` resolved via `getifaddrs`.
2. `sendto(255.255.255.255)` → EHOSTUNREACH on macOS → sent as directed broadcast on each interface.
3. UDP sockets bound to the interface address do not get broadcasts on BSD → bound to `INADDR_ANY`.
4. `closesocket` does not wake a blocked `recvfrom` and fds are reused → own never-reused handles,
   blocking calls wait with `poll` in 50 ms slices and fail with `WSAEINTR` when the socket is closed.
5. `AreWeLobbied` always returns `0x20` ("not lobbied"); DirectPlay lobbies do not exist on macOS.
6. `DirectPlayLobbyCreateA` (dplayx ordinal 4) and DirectPlay via COM fail cleanly → IPX/modem unavailable.

## Where it stopped
Joining: the UDP handshake starts (client 161 B → host 97 B → client 97 B → host 97 B), then the
client closes its discovery socket and the host's second 97-byte packet is never received; the
client stays on the JOIN GAME screen. The last change (fix 4, `poll`) was not fully re-evaluated.

Suggested next steps:
1. Run host and client (`POPTB_NET=1 tools/run.sh ... -v`, scripts below) and compare which local
   ports each socket uses before and after the client closes its discovery socket.
2. Trace the client state machine:
   `POPTB_HOOK=10011150 POPTB_TRACE_RANGE=10000000-10034000` at generation time, then
   `POPTB_SNAP_FUNC=10011150 POPTB_TRACE_ALL=1` at run time → one `tNNN.trace` per thread;
   `tools/calltree.py <trace> <game>/WEANETR.dll`.
3. Key weanetr functions: `10010600` (open UDP socket: socket, SO_BROADCAST, FIONBIO, bind,
   getsockname, local IP), `1000e420` / `1000fa30` / `1000f660` (receive loops), `100108b0`
   (`recvfrom` wrapper). Service object socket struct at `obj+0x470` (+0 port, +2 SOCKET, +6 IP).
   Exports: `JoinSession` 0x10011150, `CreateSession` 0x100110d0, `EnumerateSessions` 0x10011230.
4. Once two Macs can play, test against the original game on Windows (same network code, so the
   protocol should be compatible).

Scripts used (640x480):
- host: `17000:click:320,242;21000:click:250,172;24000:click:320,382;30000:click:320,404;34000:click:320,262`
- client (start 12 s later): `17000:click:320,242;21000:click:250,172;24000:click:320,382;36000:click:200,228;40000:click:320,374`

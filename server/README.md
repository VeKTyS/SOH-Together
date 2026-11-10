# Room server

This is what makes room codes work. It only introduces players: the host gets a six-letter
code, joiners type it in, and the server tells each game where to find the other. The games
then connect straight to each other. No gameplay traffic goes through the server, so a tiny
machine is plenty.

If two players' networks are too strict to connect directly, the game tells them to use
Tailscale. There is no relay.

Ported from the Crests of Courage room server (MIT). Theirs runs on Cloudflare Workers; this one
is a single file for plain Node.js, with no packages to install.

## Try it on your own PC

You need [Node.js](https://nodejs.org) 22 or newer.

```bash
cd server
node rooms.mjs --stun none
```

In a second terminal:

```bash
node test-flow.mjs
```

It plays a host and a joiner against the server and checks they get introduced and can reach
each other. It should end with `all good`.

To use it from the game, on the same PC: Co-op menu, Room server = `ws://127.0.0.1:8787`.

## Put it online

Any always-on machine works (a small VPS, a Raspberry Pi at home with the port forwarded...).
On that machine:

```bash
node rooms.mjs --listen 0.0.0.0 --port 8787
```

Open TCP port 8787 in its firewall. Then everyone types `ws://THAT-MACHINE'S-ADDRESS:8787` in
the game's Room server box (Co-op menu, or Advanced). Keep it running with whatever you like
(systemd, pm2, a `screen` session...).

Options (also as environment variables):

| Option | Variable | Default | What |
|---|---|---|---|
| `--listen` | `ROOMS_LISTEN` | `127.0.0.1` | Address to listen on. `0.0.0.0` = reachable from other machines |
| `--port` | `ROOMS_PORT` | `8787` | TCP port |
| `--stun` | `ROOMS_STUN` | Google + Cloudflare public STUN | `host:port,host:port`, or `none` |
| `--code` | `ROOMS_CODE` | none | Give every host this code (for tests) |

## Good to know

- **Players' IP addresses.** That's the whole job: the host gets the joiner's addresses and the
  joiner gets the host's. The game says so next to the Room codes option. The server keeps
  nothing on disk and logs no addresses.
- **STUN.** The games ask a public STUN server (by default Google's and Cloudflare's, like
  Crests of Courage) which address the internet sees them as. Those servers see the player's IP
  too. Run `--stun none` to skip it: the server then guesses the port from the WebSocket, which
  works with many home routers but not all.
- **No encryption.** The game only speaks `ws://`, not `wss://`, so room codes and addresses
  travel in clear between the game and this server. Fine for a group of friends, but don't
  advertise the server publicly.
- **Behind a reverse proxy** the server would see the proxy's address instead of the players'.
  Run it directly.
- **Limits.** 20 connections per IP, 8 joiners waiting per room, 2000 rooms, joiners dropped
  after 60 s, messages over 1 KB refused.

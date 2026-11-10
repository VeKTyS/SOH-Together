// SOH-Together room server: gives the host a room code and introduces joiners.
// Port of Crests of Courage's server (MIT): the Room part of server/src/index.js, plus the
// hand-made WebSocket code of server/dev-server.mjs so it runs on plain Node, no packages.
// Left out: Hyrule Online (the Global lobby), chat filter and crash reports.
//
// It only passes addresses around. No game traffic goes through here.
//
//   node rooms.mjs                      listens on 127.0.0.1:8787 (this PC only)
//   node rooms.mjs --listen 0.0.0.0     reachable from other computers
//   node rooms.mjs --port 9000 --stun none --code TEST22
//
// Options (or environment variables):
//   --listen ADDR   ROOMS_LISTEN   address to listen on, default 127.0.0.1
//   --port N        ROOMS_PORT     default 8787
//   --stun LIST     ROOMS_STUN     STUN servers handed to the games, "host:port,host:port" or "none".
//                                  Default: the same public ones as Crests of Courage.
//   --code CODE     ROOMS_CODE     give every host this code (handy for tests)

import { createServer } from "node:http";
import { createHash, randomBytes } from "node:crypto";
import { lookup } from "node:dns/promises";

const args = process.argv.slice(2);
const option = (name, env, fallback) => {
  const i = args.indexOf(name);
  if (i >= 0 && i + 1 < args.length) return args[i + 1];
  return process.env[env] ?? fallback;
};
const LISTEN = option("--listen", "ROOMS_LISTEN", "127.0.0.1");
const PORT = Number(option("--port", "ROOMS_PORT", "8787"));
const STUN_OPTION = option(
  "--stun",
  "ROOMS_STUN",
  "stun.l.google.com:19302,stun1.l.google.com:19302,stun.cloudflare.com:3478",
);
const FIXED_CODE = option("--code", "ROOMS_CODE", "");

// --- same constants as index.js ---
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const CODE_LENGTH = 6;
const NAME_MIN = 4;
const NAME_MAX = 24;
const MAX_JOINERS_WAITING = 8;
const HOST_KEY = /^[0-9a-f]{16,64}$/;
const MAX_MESSAGE_BYTES = 1024;

// Ours: Cloudflare protected theirs, here we keep a lid on things ourselves.
const MAX_ROOMS = 2000;
const MAX_SOCKETS_PER_IP = 20;
const JOINER_TIMEOUT_MS = 60_000; // a joiner that never finishes is dropped
const FRAME_MAX = 16 * 1024;

function newCode() {
  const bytes = randomBytes(CODE_LENGTH);
  let out = "";
  for (const b of bytes) out += CODE_ALPHABET[b % CODE_ALPHABET.length];
  return out;
}

function normalizeCode(text) {
  if (!text) return "";
  const code = text.toUpperCase().replace(/[^A-Z0-9]/g, "");
  if (code.length < NAME_MIN || code.length > NAME_MAX) return "";
  return code;
}

const newToken = () => randomBytes(8).toString("hex");

const IPV4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
const isIpv4 = (s) => typeof s === "string" && IPV4.test(s);
const isPrivateIpv4 = (s) =>
  isIpv4(s) && (/^10\./.test(s) || /^192\.168\./.test(s) || /^172\.(1[6-9]|2\d|3[01])\./.test(s));
const isTailscaleIpv4 = (s) => isIpv4(s) && /^100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\./.test(s);

function isEndpoint(s) {
  if (typeof s !== "string") return false;
  const at = s.lastIndexOf(":");
  if (at < 0) return false;
  const port = Number(s.slice(at + 1));
  return isIpv4(s.slice(0, at)) && Number.isInteger(port) && port > 0 && port < 65536;
}

const isPort = (n) => Number.isInteger(n) && n > 0 && n < 65536;

// --- STUN list: index.js asks Cloudflare's DNS over HTTPS, we use the system resolver ---
let stunCache = { at: 0, list: [] };

async function stunServers() {
  if (STUN_OPTION === "none" || STUN_OPTION === "") return [];
  if (stunCache.list.length > 0 && Date.now() - stunCache.at < 3600_000) return stunCache.list;
  const list = [];
  await Promise.all(
    STUN_OPTION.split(",").map(async (entry) => {
      const at = entry.trim().lastIndexOf(":");
      const name = entry.trim().slice(0, at);
      const port = Number(entry.trim().slice(at + 1));
      if (at < 0 || !isPort(port)) return;
      try {
        const ip = isIpv4(name) ? name : (await lookup(name, { family: 4 })).address;
        const ep = `${ip}:${port}`;
        if (isEndpoint(ep) && !list.includes(ep)) list.push(ep);
      } catch {
        // that one is down or unknown, the others will do
      }
    }),
  );
  if (list.length > 0) stunCache = { at: Date.now(), list };
  return list;
}

// --- WebSocket bits (from dev-server.mjs) ---
function writeFrame(conn, opcode, payload) {
  if (!conn || conn.destroyed) return;
  let header;
  if (payload.length < 126) {
    header = Buffer.from([0x80 | opcode, payload.length]);
  } else if (payload.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(payload.length), 2);
  }
  conn.write(Buffer.concat([header, payload]));
}

// One connected game. role: "host", "join", "replaced" or "rejected".
class Socket {
  constructor(conn, role, ip) {
    this.conn = conn;
    this.role = role;
    this.ip = ip;
    this.hello = null;
    this.key = "";
    this.paired = false;
    this.closed = false;
    this.room = null;
  }
  send(obj) {
    if (this.closed) return;
    writeFrame(this.conn, 0x1, Buffer.from(JSON.stringify(obj)));
  }
  close(code = 1000, reason = "") {
    if (this.closed) return;
    this.closed = true;
    const payload = Buffer.alloc(2 + Buffer.byteLength(reason));
    payload.writeUInt16BE(code, 0);
    payload.write(reason, 2);
    writeFrame(this.conn, 0x8, payload);
    this.conn.end();
  }
}

function fail(ws, why, detail) {
  ws.send({ op: "error", why, detail });
  ws.close(4000, why);
}

// Where the other side can be reached, best first (index.js candidates)
function candidates(info, sameNet = false, otherLan = "") {
  const out = [];
  const add = (ep) => {
    if (!out.includes(ep)) out.push(ep);
  };
  const lan = sameNet && isPrivateIpv4(info.lan) && isPort(info.port);
  if (lan) add(`${info.lan}:${info.port}`);
  if (isEndpoint(info.ep)) add(info.ep);
  if (isIpv4(info.ip) && isPort(info.port)) add(`${info.ip}:${info.port}`);
  if (sameNet && isPort(info.port) && (!lan || info.lan === otherLan)) add(`127.0.0.1:${info.port}`);
  if (isTailscaleIpv4(info.ts) && isPort(info.port)) add(`${info.ts}:${info.port}`);
  return out;
}

// --- rooms (index.js class Room, one object per code instead of a Durable Object) ---
const rooms = new Map();

class Room {
  constructor(code) {
    this.code = code;
    this.sockets = new Set();
  }

  host(except = null) {
    for (const ws of this.sockets) {
      if (ws !== except && ws.role === "host" && !ws.closed) return ws;
    }
    return null;
  }

  joiners() {
    return [...this.sockets].filter((ws) => ws.role === "join" && !ws.closed);
  }

  // Same host coming back (same key): it gets its room back, the old connection goes
  takeOver(key) {
    if (!HOST_KEY.test(key || "")) return false;
    const old = this.host();
    if (!old || old.key !== key) return false;
    old.role = "replaced";
    old.close(4001, "replaced by the same player");
    return true;
  }

  async accept(ws, { strict, key }) {
    ws.room = this;
    this.sockets.add(ws);
    if (ws.role === "host") {
      ws.key = HOST_KEY.test(key) ? key : "";
      if (strict && this.host(ws)) {
        ws.role = "rejected";
        return fail(ws, "taken");
      }
      ws.send({ op: "welcome", code: this.code, ip: ws.ip, stun: await stunServers() });
      return;
    }
    if (!this.host()) return fail(ws, "no_room");
    if (this.joiners().length > MAX_JOINERS_WAITING) return fail(ws, "busy");
    ws.send({ op: "welcome", ip: ws.ip, stun: await stunServers() });
  }

  message(ws, text) {
    if (text.length > MAX_MESSAGE_BYTES) return fail(ws, "bad_message");
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      return fail(ws, "bad_message");
    }
    if (!msg || typeof msg !== "object") return fail(ws, "bad_message");

    if (ws.role === "host") {
      if (msg.op === "hello") {
        ws.hello = {
          v: Number(msg.v) || 0,
          ep: isEndpoint(msg.ep) ? msg.ep : "",
          port: isPort(msg.port) ? msg.port : 0,
          upnp: isEndpoint(msg.upnp) ? msg.upnp : "",
          lan: isPrivateIpv4(msg.lan) ? msg.lan : "",
          ts: isTailscaleIpv4(msg.ts) ? msg.ts : "",
        };
        for (const j of this.joiners()) {
          if (j.hello && !j.paired) this.pair(ws, j);
        }
      } else if (msg.op === "update" && ws.hello) {
        ws.hello.upnp = isEndpoint(msg.upnp) ? msg.upnp : "";
      }
      return;
    }
    if (ws.role !== "join") return;

    if (msg.op !== "hello" || ws.hello) return fail(ws, "bad_message");
    ws.hello = {
      v: Number(msg.v) || 0,
      ep: isEndpoint(msg.ep) ? msg.ep : "",
      port: isPort(msg.port) ? msg.port : 0,
      lan: isPrivateIpv4(msg.lan) ? msg.lan : "",
      ts: isTailscaleIpv4(msg.ts) ? msg.ts : "",
    };
    const host = this.host();
    if (!host) return fail(ws, "no_room");
    if (host.hello) this.pair(host, ws);
  }

  pair(hostWs, joinWs) {
    const hostHello = hostWs.hello;
    const joinHello = joinWs.hello;
    if (hostHello.v !== joinHello.v) {
      hostWs.send({ op: "joiner_version", v: String(joinHello.v) });
      return fail(joinWs, "version", String(hostHello.v));
    }
    const token = newToken();
    // (index.js also hands out a key to encrypt the game traffic; our games don't use one)
    const outside = (ep) => (isEndpoint(ep) ? ep.slice(0, ep.lastIndexOf(":")) : "");
    const sameNet =
      (hostWs.ip !== "" && hostWs.ip === joinWs.ip) ||
      (outside(hostHello.ep) !== "" && outside(hostHello.ep) === outside(joinHello.ep));
    hostWs.send({
      op: "peer",
      token,
      eps: candidates({ ...joinHello, ip: joinWs.ip }, sameNet, hostHello.lan),
      sameNet,
    });
    joinWs.send({
      op: "peer",
      token,
      eps: candidates({ ...hostHello, ip: hostWs.ip }, sameNet, joinHello.lan),
      upnp: hostHello.upnp,
      sameNet,
    });
    joinWs.paired = true;
  }

  closed(ws) {
    this.sockets.delete(ws);
    // the host left: nobody can join this room any more
    if (ws.role === "host") {
      for (const j of this.joiners()) fail(j, "no_room");
    }
    if (this.sockets.size === 0 && rooms.get(this.code) === this) rooms.delete(this.code);
  }
}

function roomFor(code, create) {
  let room = rooms.get(code);
  if (!room && create) {
    room = new Room(code);
    rooms.set(code, room);
  }
  return room;
}

// --- routing (index.js fetch) ---
// Returns the Room to put the socket in, or an HTTP status to refuse with.
function route(path, query) {
  if (path === "/host") {
    const named = query.get("strict") === "1";
    const hostKey = HOST_KEY.test(query.get("key") || "") ? query.get("key") : "";
    const reclaim = normalizeCode(query.get("code")) || (named ? "" : normalizeCode(FIXED_CODE));
    if (named) {
      if (!reclaim) return { status: 400 };
      const room = roomFor(reclaim, rooms.size < MAX_ROOMS);
      if (!room) return { status: 503 };
      room.takeOver(hostKey);
      return { room, role: "host", strict: true, key: hostKey };
    }
    for (let attempt = 0; attempt < 8; ++attempt) {
      const code = attempt === 0 && reclaim ? reclaim : newCode();
      const existing = rooms.get(code);
      if (existing) existing.takeOver(hostKey);
      if (existing && existing.host()) continue; // taken, try another code
      const room = roomFor(code, rooms.size < MAX_ROOMS);
      if (!room) return { status: 503 };
      return { room, role: "host", strict: false, key: hostKey };
    }
    return { status: 503 };
  }
  const join = path.match(/^\/join\/([A-Za-z0-9-]+)$/);
  if (join) {
    const code = normalizeCode(join[1]);
    if (!code) return { status: 400 };
    // no room = nothing to create: the joiner gets "no_room" right after the upgrade
    return { room: rooms.get(code) || new Room(code), role: "join", strict: false, key: "" };
  }
  return { status: 404 };
}

const perIp = new Map();

function attach(conn, ws, head) {
  let buffer = Buffer.from(head || []);
  let gone = false;
  const done = () => {
    if (gone) return;
    gone = true;
    ws.closed = true;
    clearTimeout(ws.timer);
    perIp.set(ws.ip, (perIp.get(ws.ip) || 1) - 1);
    if (perIp.get(ws.ip) <= 0) perIp.delete(ws.ip);
    if (ws.room) ws.room.closed(ws);
  };
  const drain = () => {
    if (buffer.length > FRAME_MAX + 14) return conn.destroy();
    for (;;) {
      if (buffer.length < 2) return;
      const opcode = buffer[0] & 0x0f;
      let len = buffer[1] & 0x7f;
      let at = 2;
      if (len === 126) {
        if (buffer.length < 4) return;
        len = buffer.readUInt16BE(2);
        at = 4;
      } else if (len === 127) {
        if (buffer.length < 10) return;
        len = Number(buffer.readBigUInt64BE(2));
        at = 10;
      }
      if (len > FRAME_MAX) return conn.destroy();
      const masked = (buffer[1] & 0x80) !== 0;
      const maskAt = at;
      if (masked) at += 4;
      if (buffer.length < at + len) return;
      const payload = Buffer.from(buffer.subarray(at, at + len));
      if (masked) for (let i = 0; i < len; ++i) payload[i] ^= buffer[maskAt + (i % 4)];
      buffer = buffer.subarray(at + len);
      if (opcode === 0x1) {
        if (ws.room && !ws.closed) ws.room.message(ws, payload.toString("utf8"));
      } else if (opcode === 0x9) {
        writeFrame(conn, 0xa, payload);
      } else if (opcode === 0x8) {
        if (!ws.closed) {
          ws.closed = true;
          writeFrame(conn, 0x8, payload.subarray(0, 2));
        }
        conn.end();
        return done();
      }
    }
  };
  conn.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    drain();
  });
  conn.on("close", done);
  conn.on("error", done);
  return drain;
}

const http = createServer((req, res) => {
  res.end("SOH-Together room server. Nothing to see here.\n");
});

http.on("upgrade", async (req, conn, head) => {
  const ip = (conn.remoteAddress || "").replace(/^::ffff:/, "");
  const nope = (status) => conn.end(`HTTP/1.1 ${status} Nope\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`);
  if ((req.headers.upgrade || "").toLowerCase() !== "websocket" || !req.headers["sec-websocket-key"]) {
    return nope(400);
  }
  if ((perIp.get(ip) || 0) >= MAX_SOCKETS_PER_IP) return nope(429);
  const url = new URL(req.url, "http://localhost");
  const where = route(url.pathname, url.searchParams);
  // the full path holds the host's key: log the route only
  console.log(`[rooms] ${url.pathname.startsWith("/join/") ? "join" : url.pathname} -> ${where.status || where.role}`);
  if (where.status) return nope(where.status);

  const accept = createHash("sha1")
    .update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
    .digest("base64");
  conn.write(
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
  );
  conn.setNoDelay(true);
  const ws = new Socket(conn, where.role, ip);
  perIp.set(ip, (perIp.get(ip) || 0) + 1);
  if (where.role === "join") {
    ws.timer = setTimeout(() => ws.close(4000, "too slow"), JOINER_TIMEOUT_MS);
  }
  const drainHead = attach(conn, ws, head);
  await where.room.accept(ws, where);
  drainHead();
});

http.listen(PORT, LISTEN, () => {
  console.log(`[rooms] room server on ws://${LISTEN}:${PORT}${FIXED_CODE ? ` (code ${FIXED_CODE})` : ""}`);
});

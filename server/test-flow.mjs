// Plays a host and a joiner against a room server on this PC and checks they get introduced
// and can punch through to each other over UDP. Port of Crests of Courage's test-flow.mjs.
// Needs Node 22+ (built-in WebSocket). Start the server first, without STUN:
//   node rooms.mjs --stun none
//   node test-flow.mjs                 (or: node test-flow.mjs ws://127.0.0.1:9000)

import dgram from "node:dgram";

const SERVER = process.argv[2] || "ws://127.0.0.1:8787";
const V = 10; // like Coop::kProtocolVersion, any number works as long as both sides agree

// Same packet as the game: "CPH1" + token (8 bytes little endian) + kind
function punch(sock, token, eps, label) {
  return new Promise((resolve, reject) => {
    const tok = Buffer.from(token, "hex").reverse();
    const packet = (kind) => Buffer.concat([Buffer.from("CPH1"), tok, Buffer.from([kind])]);
    const onMsg = (msg, rinfo) => {
      if (msg.length !== 13 || msg.subarray(0, 4).toString() !== "CPH1") return;
      if (!msg.subarray(4, 12).equals(tok)) return;
      if (msg[12] === 1) sock.send(packet(2), rinfo.port, rinfo.address);
      clearInterval(timer);
      clearTimeout(giveUp);
      sock.off("message", onMsg);
      resolve(`${rinfo.address}:${rinfo.port}`);
    };
    sock.on("message", onMsg);
    const timer = setInterval(() => {
      for (const ep of eps) {
        const [ip, port] = ep.split(":");
        sock.send(packet(1), Number(port), ip);
      }
    }, 100);
    const giveUp = setTimeout(() => {
      clearInterval(timer);
      reject(new Error(`${label}: punch timed out`));
    }, 8000);
  });
}

function open(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    const inbox = [];
    const waiters = [];
    const deliver = (msg) => {
      const w = waiters.shift();
      if (w) w(msg);
      else inbox.push(msg);
    };
    ws.onmessage = (e) => deliver(JSON.parse(e.data));
    ws.onclose = () => deliver({ op: "closed" });
    ws.next = () => new Promise((res) => (inbox.length ? res(inbox.shift()) : waiters.push(res)));
    ws.onopen = () => resolve(ws);
    ws.onerror = (e) => reject(e);
  });
}

async function bindUdp() {
  const sock = dgram.createSocket("udp4");
  await new Promise((r) => sock.bind(0, "127.0.0.1", r));
  return sock;
}

function check(ok, what) {
  if (!ok) throw new Error(`FAILED: ${what}`);
  console.log(`ok - ${what}`);
}

const hostUdp = await bindUdp();
const host = await open(`${SERVER}/host?key=${"ab".repeat(16)}`);
const welcome = await host.next();
check(welcome.op === "welcome" && /^[A-Z2-9]{6}$/.test(welcome.code), `host got a room code (${welcome.code})`);
host.send(JSON.stringify({ op: "hello", v: V, ep: "", port: hostUdp.address().port, upnp: "" }));

const bad = await open(`${SERVER}/join/${welcome.code}`);
await bad.next();
bad.send(JSON.stringify({ op: "hello", v: V - 1, ep: "", port: 1234 }));
const badAnswer = await bad.next();
check(badAnswer.op === "error" && badAnswer.why === "version", "an older joiner is told to update");
const hostWarned = await host.next();
check(hostWarned.op === "joiner_version", "the host hears about it");

const named = await open(`${SERVER}/host?code=myRoom${Date.now() % 100000}&strict=1`);
const namedWelcome = await named.next();
const rival = await open(`${SERVER}/host?code=${namedWelcome.code.toLowerCase()}&strict=1`);
const rivalAnswer = await rival.next();
check(rivalAnswer.why === "taken", "a second host can't take a named room");
named.close();

const nobody = await open(`${SERVER}/join/ZZZZZZ`);
check((await nobody.next()).why === "no_room", "a wrong code gets no_room");

const joinUdp = await bindUdp();
const joiner = await open(`${SERVER}/join/${welcome.code.toLowerCase()}`);
const jw = await joiner.next();
check(jw.op === "welcome", "the joiner finds the room (lower case code works)");
joiner.send(JSON.stringify({ op: "hello", v: V, ep: "", port: joinUdp.address().port }));

const hostPeer = async () => {
  for (;;) {
    const m = await host.next();
    if (m.op === "peer") return m;
  }
};
const [toHost, toJoiner] = await Promise.all([hostPeer(), joiner.next()]);
check(toHost.token === toJoiner.token && /^[0-9a-f]{16}$/.test(toHost.token), "both got the same token");
check(toHost.sameNet && toJoiner.sameNet, "same PC counts as the same network");

const [a, b] = await Promise.all([
  punch(hostUdp, toHost.token, toHost.eps, "host"),
  punch(joinUdp, toJoiner.token, toJoiner.eps, "joiner"),
]);
check(true, `PUNCHED: host reached joiner at ${a}, joiner reached host at ${b}`);

// host comes back with the same key: same code, the old connection is replaced
const again = await open(`${SERVER}/host?code=${welcome.code}&key=${"ab".repeat(16)}`);
const againWelcome = await again.next();
check(againWelcome.code === welcome.code, "the same host gets its room back");

for (const ws of [host, joiner, again, bad, rival, nobody]) ws.close();
hostUdp.close();
joinUdp.close();
console.log("all good");
process.exit(0);

"""The ensemble on one controller (docs/DESIGN.md sections 5.7 and 10): the
controller as the master, against a stand-in node on this computer (A), or
as a node, against a stand-in master on this computer (B), over TesserAPI's
UDP link (TesserAPI's test/udp_client.py). B turns the controller's
outputs 1 and 2 on at 1 %: only with nothing wired, or Leo's agreement.
Whatever it changes (key map, slot 1, outputs 1 and 2) is restored.

    python3 test/ensemble/e2e_ensemble.py ../TesserAPI/test <ip> A|B

The stand-ins broadcast to the controller's /24 network."""
import json, sys, threading, time, urllib.request

sys.path.insert(0, sys.argv[1])
from udp_client import UdpClient, ADVERT, BYE, PORT  # noqa: E402

BOARD = sys.argv[2]
API = f"http://{BOARD}/api"
BCAST = (BOARD.rsplit(".", 1)[0] + ".255", PORT)


def http(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    r = urllib.request.Request(API + path, method=method, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=5) as f:
        t = f.read()
        return json.loads(t) if t else None


get = lambda p: http("GET", p)
put = lambda p, b: http("PUT", p, b)
pack = lambda s, a, b: s | a << 8 | b << 16
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + (f": {detail}" if detail else ""), flush=True)


class Fake(threading.Thread):
    """A TesserAPI node on this Mac: advertises, answers every request ok,
    logs sets. `hold` delays replies (s); `silent` stops everything."""

    def __init__(self, name, ident, port):
        super().__init__(daemon=True)
        self.c = UdpClient(port=port)
        self.nodeName, self.nodeId = name, ident
        self.sets, self.hold, self.silent, self.stop = [], 0.0, False, False
        self.pending = []  # (due, addr, reply)
        self.lock = threading.Lock()  # the socket, shared with request()

    def advert(self):
        p = json.dumps({"name": self.nodeName, "schema": "0000abcd", "id": self.nodeId}).encode()
        self.c._send(BCAST, 0, ADVERT, 0, 0, payload=p)

    def bye(self):
        self.c._send(BCAST, BYE, ADVERT, 0, 0)
        self.c._send(BCAST, BYE, ADVERT, 0, 0)

    def run(self):
        last = 0
        while not self.stop:
            if self.silent:
                # Drain without answering or acknowledging: as if gone.
                try:
                    self.c.sock.setblocking(False)
                    while True:
                        self.c.sock.recvfrom(2048)
                except BlockingIOError:
                    pass
                time.sleep(0.02)
                continue
            with self.lock:
                if time.time() - last > 2:
                    self.advert()
                    last = time.time()
                self.serve()
            time.sleep(0.005)

    def request(self, *a, **k):
        with self.lock:
            return self.c.request(*a, **k)

    def serve(self):
        self.c.pump(0.005)
        while self.c.messages:
            addr, env = self.c.messages.pop(0)
            if env.get("op") == "set":
                self.sets.append((time.time(), env.get("path"), env.get("body")))
            if "id" in env and "op" in env:
                self.pending.append((time.time() + self.hold, addr, {"id": env["id"], "status": "ok", "body": None}))
        now = time.time()
        due = [p for p in self.pending if p[0] <= now]
        self.pending = [p for p in self.pending if p[0] > now]
        for _, addr, reply in due:
            self.c.send(addr, reply, timeout=1.0)


def wait(pred, timeout, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pred():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False


def master_side():
    ID = "a1b2c3d4e5f6"
    node = Fake("mac-node", ID, PORT)
    node.start()
    status = lambda: {s["id"]: s for s in get("/sys/peers/status")}
    check("board finds the stand-in node", wait(lambda: status()[ID]["online"], 15), json.dumps(status().get(ID)))
    keys_before = get("/midi/keys/outputs")
    centre = pack(0xE0, 0x00, 0x40)  # the bend outlives a run: centred before and after
    try:
        http("POST", "/midi/send", centre)
        put("/ensemble/1/device", ID.upper())
        check("slot stored lowercase", get("/ensemble/1/device") == ID)
        check("slot names the node", wait(lambda: get("/ensemble/1/name") == "mac-node", 3))
        check("key map's editor names it", wait(lambda: get("/midi/keys/nodes")[0] == "mac-node", 3))
        put("/midi/keys/outputs/60", 11)
        put("/midi/keys/outputs/62", 13)

        # A note on: one patch, the node's output 1.
        n = len(node.sets)
        t0 = time.time()
        http("POST", "/midi/send", pack(0x90, 60, 127))
        t1 = time.time()
        ok = wait(lambda: len(node.sets) > n, 2, 0.002)
        s = node.sets[n] if ok else None
        check("note on arrives", ok and s[1] == "/outputs" and s[2]["1"]["on"] is True
              and abs(s[2]["1"]["frequency"] - 261.6256) < 1e-2 and abs(s[2]["1"]["duty"] - 100) < 1e-3, json.dumps(s))
        if ok:
            print(f"     request {1000*(t1-t0):.1f} ms, patch {1000*(s[0]-t1):+.1f} ms after the reply", flush=True)
        check("board's own outputs stay off", get("/outputs/1/on") is False and get("/outputs/3/on") is False)

        # Latency over 30 notes.
        lat = []
        for i in range(30):
            n = len(node.sets)
            t0 = time.time()
            http("POST", "/midi/send", pack(0x90 if i % 2 == 0 else 0x80, 62, 100))
            if wait(lambda: len(node.sets) > n, 2, 0.001):
                lat.append(1000 * (node.sets[n][0] - t0))
        lat.sort()
        check("30 notes each arrive", len(lat) == 30, f"from the request: median {lat[len(lat)//2]:.1f} ms, max {lat[-1]:.1f} ms")

        # A batch: note on, bend, note off of another, in one request; then
        # the state the node ends with.
        n = len(node.sets)
        http("POST", "/midi/send", [pack(0x90, 62, 64), pack(0xE0, 0x7F, 0x7F), pack(0x80, 60, 0)])
        wait(lambda: False, 0.5)
        state = {}
        for _, _, b in node.sets[n:]:
            for k, v in b.items():
                state.setdefault(k, {}).update(v)
        check("batch: output 1 off, output 3 on and bent", state.get("1", {}).get("on") is False
              and state.get("3", {}).get("on") is True
              and abs(state["3"]["frequency"] - 293.6648 * 2 ** (2 / 12)) < 1e-2, json.dumps(state))

        # A slow node: replies held 300 ms, 20 notes in 20 requests merge.
        node.hold = 0.3
        n = len(node.sets)
        for v in range(1, 21):
            http("POST", "/midi/send", pack(0x90, 62, v))
        http("POST", "/midi/send", pack(0x80, 62, 0))
        wait(lambda: False, 3)
        got = node.sets[n:]
        state = {}
        for _, _, b in got:
            for k, v in b.items():
                state.setdefault(k, {}).update(v)
        check("slow node: merged, latest state", len(got) < 21 and state.get("3", {}).get("on") is False
              and abs(state["3"]["duty"] - 20 * 100 / 127) < 1e-2, f"{len(got)} patches for 21 notes")
        node.hold = 0

        # The node gone silent: offline, notes dropped; back: its state.
        http("POST", "/midi/send", pack(0x90, 60, 90))
        wait(lambda: False, 0.3)
        node.silent = True
        t = time.time()
        http("POST", "/midi/send", pack(0x80, 60, 0))  # can't be delivered: the node goes offline
        off = wait(lambda: get("/ensemble/1/online") is False, 25, 0.5)
        check("silent node shows offline", off, f"after {time.time() - t:.1f} s")
        http("POST", "/midi/send", pack(0x90, 62, 127))  # dropped while offline
        n = len(node.sets)
        node.silent = False
        back = wait(lambda: len(node.sets) > n, 15, 0.05)
        state = {}
        for _, _, b in node.sets[n:]:
            for k, v in b.items():
                state.setdefault(k, {}).update(v)
        check("back online: one patch with what it plays", back and state.get("1", {}).get("on") is False
              and state.get("3", {}).get("on") is True and abs(state["3"]["duty"] - 100) < 1e-3, json.dumps(state))
        st = status()[ID]
        print("     /sys/peers status:", json.dumps(st), flush=True)
    finally:
        http("POST", "/midi/send", centre)
        http("POST", "/midi/off")
        put("/midi/keys/outputs", keys_before)
        put("/ensemble/1/device", "")
        wait(lambda: False, 0.5)
        node.stop = True
    check("restored: key map, slot, outputs off", get("/midi/keys/outputs") == keys_before
          and get("/ensemble/1/device") == "" and not any(get(f"/outputs/{i}/on") for i in range(1, 6)))


def node_side():
    before = {i: get(f"/outputs/{i}") for i in (1, 2)}
    master = Fake("mac-master", "f0e1d2c3b4a5", PORT + 1)
    master.start()
    status = lambda: {s["id"]: s for s in get("/sys/peers/status")}
    try:
        check("board finds the stand-in master", wait(lambda: status()["f0e1d2c3b4a5"]["online"], 15))
        board = (BOARD, PORT)

        def write(body):
            r = master.request(board, "set", "/outputs", body=body)
            return r and r.get("status") == "ok"

        for how in ("goodbye", "silence", "restart", "restart+data"):
            check(f"[{how}] master turns output 1 on", write({"1": {"on": True, "duty": 1, "frequency": 1000}})
                  and get("/outputs/1/on") is True)
            put("/outputs/2", {"on": True, "duty": 1, "frequency": 1000})
            t = time.time()
            if how == "goodbye":
                master.bye()
            elif how == "silence":
                master.silent = True
            else:
                # A restart: a new nonce, heard at its advertisements, and
                # in a request when "+data".
                master.stop = True
                master.join()
                master.c.close()
                master = Fake("mac-master", "f0e1d2c3b4a5", PORT + 1)
                master.start()
                if how == "restart+data":
                    master.request(board, "get", "/sys/status/uptime")
            off = wait(lambda: get("/outputs/1/on") is False, 20 if how == "restart" else 30, 0.2)
            check(f"[{how}] output 1 turned off", off, f"after {time.time() - t:.1f} s")
            check(f"[{how}] output 2, set from the page, stays on", get("/outputs/2/on") is True)
            put("/outputs/2/on", False)
            master.silent = False
            wait(lambda: status()["f0e1d2c3b4a5"]["online"], 15)
    finally:
        master.stop = True
        for i, v in before.items():
            put(f"/outputs/{i}", {"on": False, "duty": v["duty"], "frequency": v["frequency"]})
    check("restored: outputs 1 and 2 off", get("/outputs/1/on") is False and get("/outputs/2/on") is False)


(master_side if sys.argv[3] == "A" else node_side)()
print(f"DONE pass={sum(results)} fail={len(results) - sum(results)}")

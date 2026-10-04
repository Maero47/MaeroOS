"""A tiny authoritative DNS responder for the network smoke tests.

It answers A, AAAA, CNAME and PTR questions for a fixed zone over UDP on a
host port of its own, so a guest resolver pointed at it (QEMU user-net maps
10.0.2.2 to the host) resolves names with no internet access.  Everything
else gets NXDOMAIN.  Each question is logged as (name, type) in `queries`.

    dns = DnsResponder({"web.maeros.test": {"A": ["10.0.2.2"]}})
    dns.port      # the UDP port it listens on
    dns.close()
"""
import ipaddress
import socket
import struct
import threading

TYPES = {"A": 1, "NS": 2, "CNAME": 5, "PTR": 12, "AAAA": 28}
TYPE_NAMES = {v: k for k, v in TYPES.items()}


def encode_name(name):
    out = b""
    for label in name.rstrip(".").split("."):
        if label:
            out += bytes([len(label)]) + label.encode("ascii")
    return out + b"\x00"


def decode_name(msg, off):
    labels = []
    jumped = False
    end = off
    for _ in range(128):
        n = msg[off]
        if n & 0xC0 == 0xC0:
            if not jumped:
                end = off + 2
            off = ((n & 0x3F) << 8) | msg[off + 1]
            jumped = True
            continue
        off += 1
        if n == 0:
            break
        labels.append(msg[off:off + n].decode("ascii", "replace"))
        off += n
    if not jumped:
        end = off
    return ".".join(labels), end


class DnsResponder:
    def __init__(self, zone, bind="0.0.0.0"):
        """zone: {name: {"A": [ip...], "AAAA": [ip6...], "CNAME": target}}.
        PTR records are derived from the A/AAAA ones."""
        self.zone = {k.lower().rstrip("."): v for k, v in zone.items()}
        self.ptr = {}
        for name, recs in self.zone.items():
            for ip in recs.get("A", []) + recs.get("AAAA", []):
                self.ptr.setdefault(ipaddress.ip_address(ip).reverse_pointer, name)
        self.queries = []
        self.ids = []             # query IDs in arrival order
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind, 0))
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def close(self):
        self.sock.close()

    def _rr(self, name, rtype, rdata):
        return (encode_name(name) + struct.pack(">HHIH", rtype, 1, 60, len(rdata))
                + rdata)

    def _answer(self, qname, qtype):
        """(rcode, [answer RRs]) for one question, following CNAMEs."""
        name = qname.lower().rstrip(".")
        if qtype == TYPES["PTR"]:
            target = self.ptr.get(name)
            if not target:
                return 3, []
            return 0, [self._rr(qname, qtype, encode_name(target))]
        rrs = []
        for _ in range(8):
            recs = self.zone.get(name)
            if recs is None:
                return (0 if rrs else 3), rrs
            if "CNAME" in recs:
                target = recs["CNAME"].lower().rstrip(".")
                rrs.append(self._rr(name, TYPES["CNAME"], encode_name(target)))
                name = target
                continue
            if qtype == TYPES["A"]:
                for ip in recs.get("A", []):
                    rrs.append(self._rr(name, qtype, socket.inet_aton(ip)))
            elif qtype == TYPES["AAAA"]:
                for ip in recs.get("AAAA", []):
                    rrs.append(self._rr(name, qtype,
                                        socket.inet_pton(socket.AF_INET6, ip)))
            return 0, rrs                     # NOERROR, maybe no data
        return 2, []

    def _run(self):
        while True:
            try:
                msg, addr = self.sock.recvfrom(4096)
            except OSError:
                return
            try:
                ident, flags, qd = struct.unpack(">HHH", msg[:6])
                if flags & 0x8000 or qd != 1:
                    continue
                qname, off = decode_name(msg, 12)
                qtype, qclass = struct.unpack(">HH", msg[off:off + 4])
                question = msg[12:off + 4]
            except (IndexError, struct.error):
                continue
            self.queries.append((qname.lower(), TYPE_NAMES.get(qtype, qtype)))
            self.ids.append(ident)
            rcode, rrs = self._answer(qname, qtype) if qclass == 1 else (4, [])
            # QR, AA, RD copied, RA; RCODE.
            rflags = 0x8000 | 0x0400 | (flags & 0x0100) | 0x0080 | rcode
            reply = (struct.pack(">HHHHHH", ident, rflags, 1, len(rrs), 0, 0)
                     + question + b"".join(rrs))
            try:
                self.sock.sendto(reply, addr)
            except OSError:
                return

# virtio: the PCI transport and virtio-net

QEMU/KVM, most cloud VMs and most hypervisors give a guest a virtio NIC by
default. MaeroOS drives it with three pieces in `drivers/virtio/`:

| file | what it is |
|---|---|
| `virtqueue.[ch]` | split virtqueues: descriptor table, available and used rings; no PCI, no memory allocation, host-tested |
| `virtio_pci.[ch]` | the PCI transport: discovery, capabilities, reset/status/feature negotiation, queue setup, notify, MSI-X or INTx, device configuration, DMA memory |
| `virtio_net.[ch]` | the network device on top: MAC, link status, RX/TX rings, the control queue, the `netif` glue |

The transport knows nothing of what the device is, so another virtio
driver (block, GPU, input, ...) uses it unchanged; the virtio-gpu display
driver (`drivers/virtio_gpu.c`, [display.md](display.md)) is the second
user, with its control queue polled (`VIRTIO_IRQ_POLL`, no MSI vector). Its API, with the call
sequence a driver follows, is documented at the top of
`drivers/virtio/virtio_pci.h`.

## Transport

**Discovery.** `virtio_pci_find(type, n)` matches the modern ID
`1af4:(1040 + type)` and the transitional IDs `1af4:1000`-`103f` whose PCI
subsystem ID is the type (QEMU's default `virtio-net-pci` is `1af4:1000`).

**Modern interface (virtio 1.x).** The vendor-specific PCI capabilities
(ID 9) locate the common, notify, ISR and device configuration structures
inside memory BARs. Each one is mapped uncached through `mm/mmio.c`. Probe
resets the device and sets ACKNOWLEDGE and DRIVER. `set_features` writes
the 64 driver feature bits (VERSION_1 is required), sets FEATURES_OK and
checks that the device kept it. Queues are then set up, and DRIVER_OK last.
Device configuration reads are repeated until the configuration generation
stops changing.

**Legacy interface (virtio 0.9.5).** A transitional device without those
capabilities (QEMU `disable-modern=on`, older hypervisors) is driven through
its I/O BAR 0 instead: 32 feature bits, no FEATURES_OK, queue page number
registers, notify by an `outw`. The device configuration starts at 0x14, or
0x18 while MSI-X is on. A legacy queue has the device's fixed size (256 for
QEMU's virtio-net) and must be physically contiguous: descriptor table, then
the available ring, then the used ring at the next 4 KiB boundary (3 pages
for 256 entries). Those come from `pmm_alloc_contig()` (new in `mm/pmm.c`),
which finds a run of free frames inside the kernel's 256 MiB direct map. A
driver sees the same API either way. The only difference is that a legacy
device never has VIRTIO_F_VERSION_1, so virtio-net's header is 10 bytes
there instead of 12.

**Queues.** At most 256 entries (`VIRTQ_MAX_SIZE`). Modern queues take
min(device maximum, what the driver asks for), rounded down to a power of
two. Each area (16 B per descriptor, 6 + 2 B and 6 + 8 B per entry) then fits
one page from the kernel heap, so it needs no contiguous memory. Every heap
and direct-map frame is below 4 GiB (`pmm_alloc_frame`), with or without
PAE, so every DMA address the device sees is 32-bit.
`virtq_add` chains device-readable buffers then device-writable ones.
`virtq_publish` makes them visible behind a full barrier and reports whether
the device wants a notify (`VIRTQ_USED_F_NO_NOTIFY`). `virtq_get_used` takes
completions in whatever order the device finishes them and puts the chain's
descriptors back on the free list. Not used: indirect descriptors,
EVENT_IDX, packed queues.

**Interrupts.** MSI-X when the function has it: entry 0 of the table points
at the BSP's Local APIC, with one vector from the MSI pool shared by
configuration changes and every queue. The device must accept vector 0 for
the configuration and for each queue (it is read back), else the driver
falls back to INTx. INTx goes through the PIC, and a shared line is fine:
reading the ISR status register says whether the interrupt was this
device's, and acknowledges it. `virtio=intx` or `virtio=poll` on the kernel
command line forces INTx or no interrupts. The MSI pool
(`arch/i686/cpu/irq.c`) grew from 4 to 8 vectors, 0xE0-0xE7, for xHCI plus
the virtio devices.

## virtio-net

Features asked for: VERSION_1, MAC, STATUS, CTRL_VQ, CTRL_RX. Not asked
for: any checksum or segmentation offload (lwIP computes and checks every
checksum itself), MRG_RXBUF, MQ, MTU, EVENT_IDX.

- **MAC** from the device configuration. A locally administered one from
  the PCI location is used if the device offers no MAC.
- **Receive:** up to 128 buffers of 2 KiB (two per page, so a buffer never
  crosses a page), one device-writable descriptor each. Without offloads or
  GSO a frame is at most 1514 bytes plus the header, so a fixed buffer
  always holds it and MRG_RXBUF is not needed. The interrupt only wakes
  `knetd`. `net_poll_all()` drains the used ring in process context, as for
  the other NICs (lwIP is not reentrant), and gives each buffer straight
  back.
- **Transmit:** header and frame copied into one 2 KiB buffer, one
  descriptor. The transmit queue's interrupts are switched off
  (`VIRTQ_AVAIL_F_NO_INTERRUPT`). Completed buffers are reaped on the next
  send and on every poll.
- **Receive filter:** after DRIVER_OK the control queue sends
  `VIRTIO_NET_CTRL_RX_PROMISC` off and `VIRTIO_NET_CTRL_RX_ALLMULTI` on, the
  equivalent of the e1000's `RCTL.MPE`. IPv6 neighbour discovery and SLAAC
  need the solicited-node and all-nodes groups. A device without CTRL_RX
  keeps its default (QEMU's is promiscuous).
- **Link status:** a configuration-change interrupt makes the next poll
  re-read the status field. The change is logged to dmesg (`[VNET] link
  down` / `up`) and shown as `link=` in `/proc/netif`.
- **netif:** registered after `lo` as the first free `ethN`, after the
  RTL8139, e1000 and r8169 drivers have probed. With no other NIC it is
  `eth0`, ifindex 2, the interface lwIP runs on.

`/proc/netif` (and so `ifconfig`) shows the transport, interrupt mode,
features, ring sizes and counters, for example:

    eth0: virtio-net up virtio 1af4:1000 irq=msix/0xe0 mac=52:54:00:12:34:56 link=up
      features=0x00070020 rxq=128 txq=128 txfree=128 rxmode=allmulti irqs=7206
      kicks=1/51437 rxerr=0 txfull=0 linkchg=0 txpkts=58189 rxpkts=74400 drop=0 ip=10.0.2.15 ...

(`kicks=1/N`: QEMU suppresses receive-queue notifications while it has
buffers, so after the first one the driver never has to notify it.)

To try it: `make run-net NIC=virtio-net-pci`, or add
`-netdev user,id=n0 -device virtio-net-pci,netdev=n0` to any QEMU command
line (`,vectors=0` for INTx, `,disable-modern=on` for the legacy interface).

## Tests

`make smoke-net-virtio` (in `make check`, `tools/smoke_net_virtio.py`) boots
three times with `-netdev user,ipv4=on,ipv6=on` and disk.img as a snapshot:

1. **MSI-X**, modern: the boot log and `/proc/netif` show MSI-X and
   all-multicast. QEMU's `query-rx-filter` over QMP shows promiscuous off
   and multicast `all`. `busybox ip link` lists lo as 1 and eth0 as 2. Then
   come the DHCP lease and `/etc/resolv.conf` written from it, and DNS
   lookups, `httpget`, `toybox wget` and `toybox nc` by name against a
   responder in the harness (`smoke_net.dns_checks`). A 1 MiB `wget`
   checked by md5, and `toybox nc -l` plus the `srvprobe` TCP and UDP
   servers reached from the host through `hostfwd` (`smoke_tcpsrv`'s
   checks). SLAAC, `wget` over IPv6 from `[fec0::2]`, and `ping` to
   127.0.0.1 and ::1. QMP `set_link` off and on, seen by the guest as
   configuration interrupts. Finally 50 MiB each way over TCP
   (`sockprobe bulk rx|tx`), byte counts checked and rates printed, with no
   receive errors or drops afterwards.
2. **INTx** (`vectors=0`, so no MSI-X capability): DHCP, the 1 MiB wget,
   SLAAC, ping, 50 MiB each way, and interrupts actually taken.
3. **Legacy** (`disable-modern=on`) with MSI-X: 256-entry contiguous rings,
   DHCP, the 1 MiB wget, SLAAC, link off and on (the configuration at 0x18),
   50 MiB each way.

`python3 tools/smoke_net_virtio.py --bench-only e1000` runs only the
throughput part on another NIC model. `tools/test_virtqueue.py` (run by
`smoke-toybox`) builds `virtqueue.c` on the host with ASan/UBSan. It drives
the ring code against a simulated device: chain layout and flags, notify
suppression, a full ring, out-of-order completion, 70000 chains of mixed
length through a 32-entry ring (past the 16-bit index wrap), and a bogus
used entry.

Also tried by hand: `-smp 4` (MSI-X to the BSP, knetd on any CPU), a
modern-only device (`disable-legacy=on`, `1af4:1041`), legacy with INTx,
`virtio=intx` on the command line, and virtio-net next to an e1000 sharing
IRQ 11 (the e1000 is eth0, virtio-net eth1).

### Throughput

50 MiB over TCP between the guest (`sockprobe bulk`) and a host socket
through QEMU user networking, under TCG (`make check`'s QEMU settings, no
KVM, `-m 128M`, one CPU), timed by the guest's monotonic clock. Five
interleaved rounds (`--bench-only` for each model in turn), QEMU 10.2:

| NIC | receive MB/s, median (range) | send MB/s, median (range) |
|---|---|---|
| virtio-net, MSI-X | 40.9 (24-55) | 44.0 (35-51) |
| virtio-net, INTx | 49.2 (32-59) | 46.0 (39-63) |
| virtio-net, legacy, MSI-X | 44.4 (20-58) | 39.4 (30-55) |
| e1000 | 49.7 (24-53) | 47.2 (37-56) |
| RTL8139 | 45.8 (21-49) | 50.4 (47-61) |

(MB = 10^6 bytes. The host was shared with other QEMU jobs, hence the wide
ranges; a quiet run of `make smoke-net-virtio` typically prints 54-62 MB/s
both ways for virtio-net, and `--bench-only e1000` about 52/58.)

All four NICs land at about the same rate. Under TCG through slirp the
bottleneck is the guest's TCP/IP stack and the copies, not the device
model. virtio-net's advantages (fewer exits per packet, no register
emulation) would show under KVM with a tap backend, which `make check`
does not use.

## Limits

- One queue pair. No offloads, so no TSO/GSO and no checksum offload.
  No EVENT_IDX, so notification suppression is the flag kind only.
- One MSI-X vector per device, to the BSP. Interrupts are not spread over
  CPUs. The vector cannot tell a queue interrupt from a configuration
  change, so after each one the next poll re-reads the link status (one
  device-configuration read; cheap under TCG, a VM exit under KVM).
- A memory BAR above 4 GiB cannot be mapped (`mmio_map` takes 32-bit
  addresses). Such a modern device is then tried as legacy, if it is
  transitional.
- lwIP runs on `eth0` only. A virtio-net that comes up as `eth1` (behind
  an e1000, say) is registered and counted but carries no traffic.

## References

- OASIS, *Virtual I/O Device (VIRTIO) Version 1.2*, sections 2 (basic
  facilities: status, features, split virtqueues, 2.7), 3.1 (device
  initialisation), 4.1 (virtio over PCI: capabilities, common
  configuration, notifications, ISR, MSI-X vector configuration, legacy
  layout 4.1.4.8) and 5.1 (network device: features, configuration layout,
  `virtio_net_hdr`, control queue RX mode commands).
- PCI Local Bus 3.0, 6.7-6.8 (capability list, MSI-X), and BAR sizing.
- QEMU documentation for `virtio-net-pci` properties (`vectors`,
  `disable-modern`, `disable-legacy`) and the QMP commands `query-rx-filter`
  and `set_link`.
- No code was copied. Linux's `virtio_pci_modern.c` and `virtio_net.c` (GPL)
  were not used as references.

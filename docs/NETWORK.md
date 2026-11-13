# Networking

DOS-XC speaks IPv4 and IPv6 over wired Ethernet.  Install the `ethernet` package
(Setup selects it by default; on the live system, `PKG INSTALL
X:\Packages\ethernet.dsp`) and the adapter configures itself with DHCP.  The
shell announces the address before its next prompt.  IPv6 configures itself
from router advertisements (SLAAC), with a link-local address always present;
`IPCONFIG` shows both, and `PING` accepts either kind of address.  Without the
package, the driver for the adapter still starts from the driver store, and the
desktop installs the package when it finds it (see [DRIVERS.md](DRIVERS.md)).

Realtek's 2.5 Gbit/s RTL8125 and Intel's I210/I211/I225/I226 have no driver yet.

| Hardware | Driver | Tested |
|----------|--------|--------|
| QEMU, VirtualBox and VMware Intel adapters (e1000, e1000e, 82540/82545/82574) | `E1000.DRV` | QEMU e1000, e1000-82545em, e1000e |
| virtio-net, legacy and modern (QEMU/KVM, Proxmox, VirtualBox) | `VIRTIONET.DRV` | QEMU, BIOS and UEFI |
| Intel I217/I218/I219 chipset Ethernet (most Intel desktops since 2013; ThinkStation P330: I219-LM) | `I219.DRV` | Not yet run on hardware |
| Realtek RTL8111/8168, RTL8101/8102/8105/8106 and RTL8169 | `RTL8168.DRV` | Not yet run on hardware |
| Broadcom BCM5761/5764/5784, including the HP Z400 onboard port | `TG3.DRV` | Not yet run on hardware |

## Commands

```
IPCONFIG                                  adapters and addresses
IPCONFIG /RENEW                           ask the DHCP server again
IPCONFIG /STATIC ip mask gateway [dns]    fixed settings, saved in \SYSTEM\NET.CFG
IPCONFIG /DHCP                            back to DHCP
PING [/N count] host                      ICMP echo
```

Control Panel > Network does the same on the desktop, and more.  The settings
of the first adapter are in `\SYSTEM\NET.CFG`, one `KEY=value` a line:
`IP=`, `MASK=`, `GATEWAY=` and up to two `DNS=` for a fixed address;
`MODE=DHCP` with `DNS=` lines to keep those DNS servers with DHCP;
`IPV4=OFF` and `IPV6=OFF`.  Without the file the adapter uses DHCP and IPv6.
Changing the settings (`DOSXC_NET_CONFIG`) needs an administrator.

`PKG INSTALL X:\Packages\curl.dsp` adds `CURL`, which speaks HTTP and HTTPS.
Ctrl+C interrupts a program that is blocked on the network.

## TLS

`curl` and Python are linked with OpenSSL 3.5 and verify servers against the
Mozilla root certificates, `CACERT.PEM` in each package (`CURL_CA_BUNDLE` and
`SSL_CERT_FILE` point at them).  Certificate checks need the right date, so
the computer's clock must be set.  The bundle is taken when the ports are
built; rebuild the packages to refresh it.

Keys come from `getentropy`, which reads the system pool
([`os/runtime/random.c`](../os/runtime/random.c)).  It collects the timing
of every system call since boot and RDRAND where the processor has it.  The
pool is not saved across restarts, so on a processor without RDRAND a program
started straight after boot gets less randomness than one started later.

## For programs

The C library provides BSD sockets (`<sys/socket.h>`, `<netinet/in.h>`,
`<arpa/inet.h>`, `<netdb.h>`), TCP and UDP, blocking and non-blocking, with
`poll` and `select`.  `getaddrinfo` resolves names with the DHCP-supplied DNS
servers.  `127.0.0.1` and `::1` reach the machine itself.  An `AF_INET6` socket
also talks IPv4 through mapped addresses unless `IPV6_V6ONLY` is set.
`getaddrinfo` asks for AAAA records only when an adapter has a global IPv6
address, and then lists IPv6 addresses first.

Limits: no IP fragment reassembly, 64 KiB TCP windows, no DHCPv6, one global
IPv6 address per adapter (the interface identifier is random for each boot),
and link-local addresses always use the first adapter because scope ids are
ignored.

## Firewall

The runtime checks every connection, datagram, bound port and listening
socket against `\SYSTEM\FIREWALL.CFG` (`os/runtime/firewall.c`); the
Firewall program on the desktop edits it.  One rule per line:

```
enabled=1
# action direction protocol ports program
allow out any * DOSFOX.EXE
allow in tcp 8000-8080 PYTHON.EXE
block out udp 53 *
```

The first rule that matches decides; anything no rule allows is blocked with
`EACCES` and recorded in Event Logs.  Programs are matched by file name.
Loopback traffic and the system's own traffic (DHCP, address configuration
and `PING`) are not filtered.  Without the file the firewall is
on and allows DOSfox, curl, Python and PING to send.  Installed packages
add their own programs: each `Firewall=A.EXE;B.EXE` line of a
`\Programs\<Name>\PACKAGE.INF` becomes `allow out any * A.EXE` after the
file's rules (so a rule in the file can still block them), and PKG reloads
the rules when it installs or removes such a package.  `enabled=0` turns it off;
`DOSXC_NET_FIREWALL` reloads the file (administrators).

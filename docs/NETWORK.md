# Networking

DOS-XC speaks IPv4 over wired Ethernet.  Install the `ethernet` package
(Setup selects it by default; on the live system, `PKG INSTALL
X:\Packages\ethernet.dsp`) and the adapter configures itself with DHCP.  The
shell announces the address before its next prompt.

| Hardware | Driver | Tested |
|----------|--------|--------|
| QEMU, VirtualBox and VMware Intel adapters (e1000, e1000e, 82540/82545/82574) | `E1000.DRV` | QEMU e1000, e1000-82545em, e1000e |
| virtio-net, legacy and modern (QEMU/KVM, Proxmox, VirtualBox) | `VIRTIONET.DRV` | QEMU, BIOS and UEFI |
| Broadcom BCM5761/5764/5784, including the HP Z400 onboard port | `TG3.DRV` | Not yet run on hardware |

## Commands

```
IPCONFIG                                  adapters and addresses
IPCONFIG /RENEW                           ask the DHCP server again
IPCONFIG /STATIC ip mask gateway [dns]    fixed settings, saved in \SYSTEM\NET.CFG
IPCONFIG /DHCP                            back to DHCP
PING [/N count] host                      ICMP echo
```

`PKG INSTALL X:\Packages\curl.dsp` adds `CURL` (HTTP only).  Ctrl+C
interrupts a program that is blocked on the network.

## For programs

The C library provides BSD sockets (`<sys/socket.h>`, `<netinet/in.h>`,
`<arpa/inet.h>`, `<netdb.h>`), TCP and UDP, blocking and non-blocking, with
`poll` and `select`.  `getaddrinfo` resolves names with the DHCP-supplied DNS
servers.  `127.0.0.1` reaches the machine itself.

Limits: IPv4 only, no IP fragment reassembly, 64 KiB TCP windows.  There is no
TLS library yet, so `https://` URLs do not work.

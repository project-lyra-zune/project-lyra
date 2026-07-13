#ifndef MDNS_H
#define MDNS_H

// One discovered Cast receiver. `name` is the friendly name (TXT "fn="), ASCII;
// `ip` is dotted IPv4; `port` is the per-device SRV control port (8009 for an
// individual receiver, a dynamic high port for a Cast group).
typedef struct {
    char           ip[24];
    unsigned short port;
    char           name[64];
} MdnsDevice;

// Enumerates every Cast responder on the LAN over `timeout_ms`, filling up to
// `max` entries (dedup on ip:port: a group and its host member share an IPv4
// but differ by port). Returns the number written. The device list backing the
// HUD picker.
int mdns_enumerate_chromecast(MdnsDevice* out, int max, int timeout_ms);

// Discovers Cast receivers via one-shot mDNS query for _googlecast._tcp.local,
// logging every responder as "name @ ip:port". Writes the first responder's IPv4
// (dotted string) into out_ip and its SRV control port into *out_port. A
// per-device port is mandatory: individual receivers answer on 8009, but a Cast
// group (md=Google Cast Group) runs its virtual receiver on a dynamic high port.
// Returns 1 on success, 0 on timeout/failure. Caller falls back to a configured
// address + the standard 8009 control port.
int mdns_discover_chromecast(char* out_ip, int out_ip_sz,
                             unsigned short* out_port, int timeout_ms);

#endif // MDNS_H

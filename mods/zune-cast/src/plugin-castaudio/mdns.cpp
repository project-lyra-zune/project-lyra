#include "castaudio.h"
#include "mdns.h"
#include <stdio.h>
#include <string.h>

#define MDNS_GROUP  "224.0.0.251"
#define MDNS_PORT   5353

// Builds a standard mDNS query: 1 question, PTR for _googlecast._tcp.local,
// QCLASS = IN with the unicast-response (QU) bit set so responders reply
// directly to our ephemeral port (no group join / port-5353 bind needed).
static int build_query(unsigned char* q)
{
    memset(q, 0, 12);
    q[5] = 1;                                   // QDCOUNT = 1
    int n = 12;
    static const char* labels[] = { "_googlecast", "_tcp", "local" };
    for (int i = 0; i < 3; i++) {
        int l = (int)strlen(labels[i]);
        q[n++] = (unsigned char)l;
        memcpy(q + n, labels[i], l);
        n += l;
    }
    q[n++] = 0;                                 // root label
    q[n++] = 0; q[n++] = 12;                    // QTYPE = PTR
    q[n++] = 0x80; q[n++] = 0x01;               // QCLASS = QU | IN
    return n;
}

// Advance past a DNS name, honoring compression pointers (a pointer ends the name).
static int skip_name(const unsigned char* p, int len, int pos)
{
    while (pos < len) {
        unsigned char b = p[pos];
        if (b == 0) return pos + 1;
        if ((b & 0xc0) == 0xc0) return pos + 2;
        pos += 1 + b;
    }
    return pos;
}

// Extracts the device's IPv4 (first A record), SRV control port, and friendly
// name (TXT "fn=") from one responder's packet. Per-packet parsing keeps the A,
// SRV, and TXT records correlated to the same device: load-bearing for groups,
// whose SRV port differs from their host member's. Returns 1 if an IPv4 was found.
static int parse_packet(const unsigned char* p, int len,
                        char* ip, int ipsz, char* name, int namesz,
                        unsigned short* port)
{
    if (name && namesz) name[0] = '\0';
    if (port) *port = 0;
    if (len < 12) return 0;
    int qd = (p[4] << 8) | p[5];
    int an = (p[6] << 8) | p[7];
    int ns = (p[8] << 8) | p[9];
    int ar = (p[10] << 8) | p[11];
    // Skip exactly QDCOUNT questions (each = name + QTYPE + QCLASS). A spec
    // mDNS response has QDCOUNT=0 (RFC 6762 §6); some devices echo the question
    // (QDCOUNT=1). Assuming one question dropped every QDCOUNT=0 responder (e.g.
    // a Chromecast-built-in AVR), misaligning its records into nothing parsed.
    int pos = 12;
    for (int i = 0; i < qd; i++) pos = skip_name(p, len, pos) + 4;
    int total = an + ns + ar, have_ip = 0;
    for (int i = 0; i < total; i++) {
        pos = skip_name(p, len, pos);
        if (pos + 10 > len) break;
        int type  = (p[pos] << 8) | p[pos + 1];
        int rdlen = (p[pos + 8] << 8) | p[pos + 9];
        int rdp = pos + 10;
        if (rdp + rdlen > len) break;
        if (type == 1 && rdlen == 4 && !have_ip) {           // A
            _snprintf(ip, ipsz, "%u.%u.%u.%u",
                      p[rdp], p[rdp + 1], p[rdp + 2], p[rdp + 3]);
            have_ip = 1;
        } else if (type == 33 && rdlen >= 6 && port && !*port) {  // SRV
            // RFC 2782 rdata: priority(2) weight(2) port(2) target(name).
            *port = (unsigned short)((p[rdp + 4] << 8) | p[rdp + 5]);
        } else if (type == 16 && name && !name[0]) {         // TXT: find fn=
            int t = rdp;
            while (t < rdp + rdlen) {
                int slen = p[t++];
                if (t + slen > rdp + rdlen) break;
                if (slen > 3 && memcmp(p + t, "fn=", 3) == 0) {
                    int cp = slen - 3; if (cp > namesz - 1) cp = namesz - 1;
                    memcpy(name, p + t + 3, cp); name[cp] = '\0';
                }
                t += slen;
            }
        }
        pos = rdp + rdlen;
    }
    return have_ip;
}

int mdns_enumerate_chromecast(MdnsDevice* out, int max, int timeout_ms)
{
    if (!out || max <= 0) return 0;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) { cast_log("MDNS socket fail %d", WSAGetLastError()); return 0; }

    int ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, sizeof(ttl));  // best-effort

    unsigned char q[64];
    int qn = build_query(q);

    sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    if (sendto(s, (const char*)q, qn, 0, (sockaddr*)&dst, sizeof(dst)) == SOCKET_ERROR) {
        cast_log("MDNS sendto fail %d", WSAGetLastError());
        closesocket(s);
        return 0;
    }

    // Enumerate all responders for the full window (the HUD device list). Dedup
    // on ip:port: a group and its host member share an IPv4, so keying on ip
    // alone would drop the group as a duplicate of its member receiver. A single
    // query drops packets and misses slower responders, so re-query periodically
    // across the whole window (~every 750 ms) rather than once.
    int   found = 0;
    DWORD start = GetTickCount();
    DWORD last_query = start;
    while (GetTickCount() - start < (DWORD)timeout_ms && found < max) {
        if (GetTickCount() - last_query >= 750) {
            sendto(s, (const char*)q, qn, 0, (sockaddr*)&dst, sizeof(dst));  // best-effort re-query
            last_query = GetTickCount();
        }
        fd_set rf;
        FD_ZERO(&rf); FD_SET(s, &rf);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        if (select(0, &rf, NULL, NULL, &tv) <= 0) continue;

        unsigned char buf[1500];
        sockaddr_in from; int fl = sizeof(from);
        int n = recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;

        char ip[32], name[64];
        unsigned short port = 0;
        if (!parse_packet(buf, n, ip, sizeof(ip), name, sizeof(name), &port)) continue;
        if (port == 0) continue;                // need the SRV port to reach this receiver

        int dup = 0;
        for (int i = 0; i < found; i++)
            if (out[i].port == port && strcmp(out[i].ip, ip) == 0) { dup = 1; break; }
        if (dup) continue;

        _snprintf(out[found].ip, sizeof(out[found].ip), "%s", ip);
        out[found].port = port;
        _snprintf(out[found].name, sizeof(out[found].name), "%s", name[0] ? name : ip);
        cast_log("MDNS dev '%s' @ %s:%u", out[found].name, ip, port);
        found++;
    }

    closesocket(s);
    return found;
}

int mdns_discover_chromecast(char* out_ip, int out_ip_sz,
                             unsigned short* out_port, int timeout_ms)
{
    MdnsDevice devs[8];
    int n = mdns_enumerate_chromecast(devs, 8, timeout_ms);
    if (n <= 0) return 0;
    _snprintf(out_ip, out_ip_sz, "%s", devs[0].ip);
    if (out_port) *out_port = devs[0].port;
    return 1;
}

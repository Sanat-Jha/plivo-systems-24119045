/* SENDER (C++) — forward redundancy + budget-guarded NACK retransmits.
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source: 4B big-endian seq + 160B payload, every 20 ms
 *   send 47001  -> relay uplink (our wire format, see below)
 *   bind 47004  <- NACKs from our receiver, via the relay
 *
 * Wire format (media, ours):
 *   [4B seq BE][1B flags][160B payload of seq][160B aux block if flags != 0]
 *   flags: 0         = no aux block
 *          1..7 (D)  = aux is a plain copy of frame seq-D
 *          8..13     = aux is XOR of frames (seq-1, seq-S), S = flags-6
 *
 * XOR parity beats a plain copy at identical cost: a lost frame m can be
 * rebuilt from packet m+1 OR m+S (two chances, not one). Spreading the
 * second cover (S > 2) makes loss BURSTS decodable: a burst of up to S
 * consecutive lost packets is fully recovered by back-substitution through
 * later parities, while a single loss still decodes from the very next
 * packet. Every SKIPth frame (seq % SKIP == 0) carries no aux: full
 * duplication of 160B payloads cannot fit the 2.0x byte cap (2*160 = 320 =
 * exactly 2.0x before any header), so we skip ~1/16 of aux blocks and stay
 * ~1.97x.
 *
 * Feedback packets (from receiver, port 47004):
 *   [1B count][count x 4B seq BE]  count 1..32  = NACK, resend those frames
 *   [0xFF][1B S]                                = adaptive-spread advisory
 *
 * NACK'd frames within 7 seqs of each other are BUNDLED two per resend
 * packet using the copy-block wire format (primary = higher seq, aux = plain
 * copy of the lower) -- one header instead of two, so the byte budget funds
 * more retries. Resends are rate-limited per seq and gated by a global byte
 * budget so overhead can never cross the cap.
 *
 * The advisory closes an adaptation loop: the receiver observes loss-burst
 * lengths and asks for a wider parity spread S; the sender only ever widens
 * (never shrinks), and the receiver sees the new S in the parity flags of
 * subsequent packets, which stops the advisories.
 *
 * Env: T0, DURATION_S, DELAY_MS (harness). Tuning knobs (optional):
 *   FLAKY_MODE (xor|pb, default xor), FLAKY_S (starting spread, default 3),
 *   FLAKY_D (default 1), FLAKY_SKIP (16), FLAKY_DEBUG (0).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

static const int PAYLOAD = 160;
static const size_t HISTORY = 1 << 16;  // ring of frames, indexed seq % HISTORY

struct Slot {
    uint32_t seq = 0;
    bool valid = false;
    double last_resend = 0.0;
    unsigned char payload[PAYLOAD];
};

static double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v ? atoi(v) : def;
}

static int udp_bind(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        exit(1);
    }
    return fd;
}

static struct sockaddr_in dest(uint16_t port) {
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    return a;
}

static double env_f(const char *name, double def) {
    const char *v = getenv(name);
    return v ? atof(v) : def;
}

int main() {
    const char *mode_s = getenv("FLAKY_MODE");
    const bool xor_mode = !(mode_s && strcmp(mode_s, "pb") == 0);
    int S = env_int("FLAKY_S", 3);              // xor spread: covers seq-1, seq-S
    if (S < 2) S = 2;
    if (S > 7) S = 7;
    const bool dbg = env_int("FLAKY_DEBUG", 0) != 0;
    const int D = env_int("FLAKY_D", 1);        // copy offset in pb mode (1..7)
    const int SKIP = env_int("FLAKY_SKIP", 16); // skip aux when seq%SKIP==0
    // last frame of the run has no successor packet to piggyback it: send twice
    const uint32_t last_seq = (uint32_t)(env_f("DURATION_S", 30.0) * 50) - 1;

    int in_fd = udp_bind(47010);
    int fb_fd = udp_bind(47004);
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = dest(47001);

    std::vector<Slot> hist(HISTORY);
    uint64_t frames_seen = 0;
    uint64_t bytes_sent = 0;  // everything we hand the relay (counts even if dropped)

    struct pollfd pfds[2] = {{in_fd, POLLIN, 0}, {fb_fd, POLLIN, 0}};
    unsigned char buf[2048];
    unsigned char pkt[8 + 2 * PAYLOAD];

    for (;;) {
        if (poll(pfds, 2, 1000) <= 0) continue;

        if (pfds[0].revents & POLLIN) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n == 4 + PAYLOAD) {
                uint32_t seq_n;
                memcpy(&seq_n, buf, 4);
                uint32_t seq = ntohl(seq_n);

                Slot &s = hist[seq % HISTORY];
                s.seq = seq;
                s.valid = true;
                s.last_resend = 0.0;
                memcpy(s.payload, buf + 4, PAYLOAD);
                frames_seen++;

                // media packet: header + this frame (+ aux block)
                memcpy(pkt, buf, 4);  // seq already big-endian from harness
                unsigned char flags = 0;
                size_t len = 5 + PAYLOAD;
                memcpy(pkt + 5, buf + 4, PAYLOAD);
                if (seq >= 1 && seq % SKIP != 0) {
                    Slot &p1 = hist[(seq - 1) % HISTORY];
                    if (xor_mode && seq >= (uint32_t)S) {
                        Slot &p2 = hist[(seq - S) % HISTORY];
                        if (p1.valid && p1.seq == seq - 1 &&
                            p2.valid && p2.seq == seq - (uint32_t)S) {
                            flags = (unsigned char)(6 + S);  // XOR(seq-1, seq-S)
                            for (int b = 0; b < PAYLOAD; b++)
                                pkt[len + b] = p1.payload[b] ^ p2.payload[b];
                            len += PAYLOAD;
                        }
                    } else if (seq >= (uint32_t)D) {
                        Slot &p = hist[(seq - D) % HISTORY];
                        if (p.valid && p.seq == seq - (uint32_t)D) {
                            flags = (unsigned char)(D & 7);  // plain copy
                            memcpy(pkt + len, p.payload, PAYLOAD);
                            len += PAYLOAD;
                        }
                    }
                }
                pkt[4] = flags;
                sendto(out_fd, pkt, len, 0, (struct sockaddr *)&relay, sizeof relay);
                bytes_sent += len;
                if (seq == last_seq) {  // second, independent chance for the tail
                    sendto(out_fd, pkt, len, 0, (struct sockaddr *)&relay,
                           sizeof relay);
                    bytes_sent += len;
                }
            }
        }

        if (pfds[1].revents & POLLIN) {
            ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n == 2 && buf[0] == 0xFF) {
                // adaptive-spread advisory: widen S (never shrink)
                int sug = buf[1];
                if (sug > S && sug <= 7) {
                    if (dbg)
                        fprintf(stderr, "[sender] widening spread S %d -> %d\n",
                                S, sug);
                    S = sug;
                }
            } else if (n >= 1 && buf[0] >= 1 && buf[0] <= 32 &&
                       n == 1 + 4 * (int)buf[0]) {
                // NACK: collect resendable seqs, then send bundled pairs
                double t = now_s();
                int count = buf[0];
                uint32_t list[32];
                int m = 0;
                for (int k = 0; k < count; k++) {
                    uint32_t seq_n;
                    memcpy(&seq_n, buf + 1 + 4 * k, 4);
                    uint32_t seq = ntohl(seq_n);
                    Slot &s = hist[seq % HISTORY];
                    if (!s.valid || s.seq != seq) continue;
                    if (t - s.last_resend < 0.040) continue;  // per-seq rate limit
                    list[m++] = seq;
                }
                for (int i = 0; i < m;) {
                    // pair adjacent requests when the copy block can reach
                    bool pair = i + 1 < m && list[i + 1] > list[i] &&
                                list[i + 1] - list[i] <= 7;
                    size_t len = pair ? 5 + 2 * PAYLOAD : 5 + PAYLOAD;
                    // budget guard: never let total bytes cross ~1.98x raw so far
                    if (bytes_sent + len > (uint64_t)(1.98 * frames_seen * PAYLOAD))
                        break;
                    uint32_t primary = pair ? list[i + 1] : list[i];
                    Slot &sp = hist[primary % HISTORY];
                    uint32_t be = htonl(primary);
                    memcpy(pkt, &be, 4);
                    memcpy(pkt + 5, sp.payload, PAYLOAD);
                    if (pair) {
                        uint32_t d = list[i + 1] - list[i];
                        Slot &sl = hist[list[i] % HISTORY];
                        pkt[4] = (unsigned char)d;  // aux = copy of primary-d
                        memcpy(pkt + 5 + PAYLOAD, sl.payload, PAYLOAD);
                        sl.last_resend = t;
                    } else {
                        pkt[4] = 0;
                    }
                    sp.last_resend = t;
                    sendto(out_fd, pkt, len, 0, (struct sockaddr *)&relay,
                           sizeof relay);
                    bytes_sent += len;
                    if (dbg) {
                        if (pair)
                            fprintf(stderr, "[sender] resend pair %u+%u\n",
                                    list[i], primary);
                        else
                            fprintf(stderr, "[sender] resend %u\n", primary);
                    }
                    i += pair ? 2 : 1;
                }
            }
        }
    }
    return 0;
}

/* RECEIVER (C++) — dedupe + immediate forward + XOR-parity decode + optional NACK.
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from our sender via the relay (our wire format)
 *   send 47020  -> harness player: 4B big-endian seq + 160B payload
 *   send 47003  -> NACKs to our sender, via the relay (optional, FLAKY_NACK=1)
 *
 * The harness player records the FIRST arrival per seq and judges its time
 * against the deadline; order does not matter. So there is no playout clock
 * here: every frame (received, decoded, or resent) is forwarded to the
 * player the moment it is first known, and duplicates are suppressed.
 *
 * Media packet: [4B seq BE][1B flags][160B payload][160B aux if flags != 0]
 *   flags 1..7 (D): aux = plain copy of frame seq-D
 *   flags 8..13:    aux = XOR of frames (seq-1, seq-S), S = flags-6
 * A stored parity from packet k covers frames (k-1, k-S); whenever one of
 * the two becomes known the other is decoded, which can cascade (worklist),
 * so a burst of up to S lost packets is recovered by chaining parities.
 *
 * Env: T0, DURATION_S, DELAY_MS (harness). Tuning knobs (optional):
 *   FLAKY_NACK (default 1), FLAKY_NACK_WAIT_MS (25), FLAKY_NACK_MAX (2).
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

static double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v ? atoi(v) : def;
}

static double env_f(const char *name, double def) {
    const char *v = getenv(name);
    return v ? atof(v) : def;
}

static int udp_bind(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind 47002");
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

struct Track {
    double missing_since = 0.0;  // 0 = not yet noticed missing
    double last_nack = 0.0;
    int nacks = 0;
};

int main() {
    const double t0 = env_f("T0", 0.0);
    const double delay_s = env_f("DELAY_MS", 60.0) / 1000.0;
    const double dur = env_f("DURATION_S", 30.0);
    const bool nack_on = env_int("FLAKY_NACK", 1) != 0;
    const double nack_wait = env_int("FLAKY_NACK_WAIT_MS", 25) / 1000.0;
    const int nack_max = env_int("FLAKY_NACK_MAX", 2);

    const size_t max_frames = (size_t)(dur * 50) + 256;

    std::vector<unsigned char> data(max_frames * PAYLOAD);  // known payloads
    std::vector<bool> have(max_frames, false);
    // parity from packet k covers (k-1, k-S); indexed by k
    std::vector<unsigned char> pdata(max_frames * PAYLOAD);
    std::vector<bool> phave(max_frames, false);
    std::vector<uint8_t> pspread(max_frames, 0);  // S of each stored parity
    int s_seen = 2;  // sender's spread, learned from parity flags
    std::vector<Track> track(max_frames);
    std::vector<uint32_t> work;  // newly-known frames to run against parities

    int in_fd = udp_bind(47002);
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = dest(47020);
    struct sockaddr_in fb = dest(47003);

    int64_t highest = -1;  // highest seq seen in any packet
    double last_nack_send = 0.0;

    unsigned char buf[2048];
    unsigned char out[4 + PAYLOAD];
    unsigned char nackpkt[1 + 4 * 32];

    struct pollfd pfd = {in_fd, POLLIN, 0};

    // forward a newly-known frame to the player and queue it for decoding
    auto learn = [&](uint32_t seq, const unsigned char *payload) {
        if (seq >= max_frames || have[seq]) return;
        memcpy(&data[seq * PAYLOAD], payload, PAYLOAD);
        have[seq] = true;
        uint32_t be = htonl(seq);
        memcpy(out, &be, 4);
        memcpy(out + 4, payload, PAYLOAD);
        sendto(out_fd, out, sizeof out, 0, (struct sockaddr *)&player, sizeof player);
        work.push_back(seq);
    };

    // parity covering (a=k-1, b=k-S): if exactly one is known, decode the other
    auto try_parity = [&](uint32_t k) {
        if (k >= max_frames || !phave[k]) return;
        uint32_t a = k - 1, b = k - pspread[k];
        if (have[a] == have[b]) {
            if (have[a]) phave[k] = false;  // both known: parity is spent
            return;
        }
        uint32_t known = have[a] ? a : b, other = have[a] ? b : a;
        unsigned char dec[PAYLOAD];
        for (int i = 0; i < PAYLOAD; i++)
            dec[i] = pdata[k * PAYLOAD + i] ^ data[known * PAYLOAD + i];
        phave[k] = false;
        learn(other, dec);
    };

    auto drain_work = [&]() {
        while (!work.empty()) {
            uint32_t f = work.back();
            work.pop_back();
            try_parity(f + 1);       // parity in packet f+1 covers f as "k-1"
            try_parity(f + s_seen);  // parity in packet f+S covers f as "k-S"
        }
    };

    for (;;) {
        int r = poll(&pfd, 1, 5);
        double t = now_s();

        if (r > 0 && (pfd.revents & POLLIN)) {
            for (;;) {  // drain everything that's ready
                ssize_t n = recvfrom(in_fd, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL);
                if (n < 5) break;
                uint32_t seq_n;
                memcpy(&seq_n, buf, 4);
                uint32_t seq = ntohl(seq_n);
                unsigned char flags = buf[4];

                if (n >= 5 + PAYLOAD) learn(seq, buf + 5);
                if (flags != 0 && n >= 5 + 2 * PAYLOAD) {
                    if (flags >= 1 && flags <= 7) {          // plain copy of seq-D
                        if (seq >= flags) learn(seq - flags, buf + 5 + PAYLOAD);
                    } else if (flags >= 8 && flags <= 13) {  // XOR(seq-1, seq-S)
                        int s = flags - 6;
                        s_seen = s;
                        if (seq >= (uint32_t)s && seq < max_frames && !phave[seq]) {
                            memcpy(&pdata[seq * PAYLOAD], buf + 5 + PAYLOAD,
                                   PAYLOAD);
                            phave[seq] = true;
                            pspread[seq] = (uint8_t)s;
                            try_parity(seq);
                        }
                    }
                }
                drain_work();

                if ((int64_t)seq > highest) {
                    // frames between old highest and seq are now known-missing
                    for (int64_t m = highest + 1; m < (int64_t)seq && m >= 0; m++)
                        if ((size_t)m < max_frames && !have[m] &&
                            track[m].missing_since == 0.0)
                            track[m].missing_since = t;
                    highest = seq;
                }
            }
        }

        if (nack_on && highest >= 0 && t - last_nack_send >= 0.020) {
            int count = 0;
            int64_t lo = highest - 256 < 0 ? 0 : highest - 256;
            for (int64_t m = lo; m < highest && count < 32; m++) {
                Track &tr = track[m];
                if (have[m] || tr.missing_since == 0.0) continue;
                if (t - tr.missing_since < nack_wait) continue;      // reorder guard
                if (tr.nacks >= nack_max) continue;
                if (t - tr.last_nack < 0.040) continue;
                // deadline still reachable? need NACK + resend to cross the relay
                double deadline = t0 + delay_s + m * 0.020;
                if (t + 0.040 > deadline) continue;
                uint32_t be = htonl((uint32_t)m);
                memcpy(nackpkt + 1 + 4 * count, &be, 4);
                count++;
                tr.last_nack = t;
                tr.nacks++;
            }
            if (count > 0) {
                nackpkt[0] = (unsigned char)count;
                sendto(out_fd, nackpkt, 1 + 4 * count, 0,
                       (struct sockaddr *)&fb, sizeof fb);
                last_nack_send = t;
            }
        }
    }
    return 0;
}

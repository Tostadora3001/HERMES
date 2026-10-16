/* Reliable packets over a serial port: the protocol (see hermes_packet.h
 * and docs/raw/hermes_packet.en.md).
 *
 * A frame, as it goes on the wire:
 *   0x7E | type | seq | len | payload (len bytes) | CRC16 high | CRC16 low
 * 0x7E starts every frame; inside one, 0x7E and 0x7D are sent as 0x7D
 * followed by the byte XOR 0x20 (byte stuffing, as in HDLC/PPP), so a
 * receiver that lost bytes finds the next frame at the next 0x7E. The CRC
 * (CRC-16/CCITT-FALSE: polynomial 0x1021, initial 0xFFFF) covers type,
 * seq, len and the payload. Types: DATA (1) and ACK (2, no payload).
 *
 * Stop-and-wait: a DATA frame with sequence s is sent again every
 * `timeout_ms` until an ACK with s comes (at most 1 + `retries` times).
 * The receiver acknowledges a good DATA frame when it takes it -- only if
 * the program isn't still holding the previous packet (flow control: no
 * ACK, so the sender tries again later) -- and acknowledges again,
 * without delivering it twice, a DATA frame with the sequence it already
 * delivered last (its ACK was lost). A frame with a bad CRC is dropped. */
#include "hermes_packet.h"
#include "hermes_drv_uart.h"
#include "hermes_drv_timer.h"
#include "hermes_sched_internal.h" /* hermes_lock */

#define FLAG 0x7E
#define ESC  0x7D
#define T_DATA 1
#define T_ACK  2

enum { RX_IDLE, RX_TYPE, RX_SEQ, RX_LEN, RX_BODY };

static uint16_t crc_add(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    return crc;
}

static void put_stuffed(int port, uint8_t b) {
    if (b == FLAG || b == ESC) {
        hermes_uart_port_write_byte(port, ESC);
        b ^= 0x20;
    }
    hermes_uart_port_write_byte(port, b);
}

static void send_frame(hermes_packet_state_t *p, uint8_t type, uint8_t seq, const uint8_t *data, uint8_t len) {
    uint16_t crc = 0xFFFF;
    crc = crc_add(crc, type);
    crc = crc_add(crc, seq);
    crc = crc_add(crc, len);
    for (int i = 0; i < len; i++) crc = crc_add(crc, data[i]);
    hermes_uart_port_write_byte(p->port, FLAG);
    put_stuffed(p->port, type);
    put_stuffed(p->port, seq);
    put_stuffed(p->port, len);
    for (int i = 0; i < len; i++) put_stuffed(p->port, data[i]);
    put_stuffed(p->port, (uint8_t)(crc >> 8));
    put_stuffed(p->port, (uint8_t)crc);
}

/* A whole frame arrived (type, seq, len and payload + CRC in p->rx). */
static void frame_done(hermes_packet_state_t *p) {
    uint16_t crc = 0xFFFF;
    crc = crc_add(crc, p->rx_type);
    crc = crc_add(crc, p->rx_seq);
    crc = crc_add(crc, p->rx_len);
    for (int i = 0; i < p->rx_len; i++) crc = crc_add(crc, p->rx[i]);
    if (crc != (uint16_t)((p->rx[p->rx_len] << 8) | p->rx[p->rx_len + 1])) return; /* damaged */
    if (p->rx_type == T_ACK) {
        if (p->rx_seq == p->tx_seq) p->acked = 1;
    } else if (p->rx_type == T_DATA) {
        if (p->have_last && p->rx_seq == p->last_seq) {
            send_frame(p, T_ACK, p->rx_seq, 0, 0); /* a resend: our ACK was lost */
        } else if (!p->in_ready && !p->in_read) {
            for (int i = 0; i < p->rx_len; i++) p->in[i] = p->rx[i];
            p->in_len = p->rx_len;
            p->in_ready = 1;
            p->last_seq = p->rx_seq;
            p->have_last = 1;
            send_frame(p, T_ACK, p->rx_seq, 0, 0);
        } /* else: the program still holds a packet -- no ACK, it comes again */
    }
}

/* Every byte waiting on the port, through the frame decoder. */
static void pump(hermes_packet_state_t *p) {
    int c;
    while ((c = hermes_uart_port_read_byte(p->port)) >= 0) {
        uint8_t b = (uint8_t)c;
        if (b == FLAG) { p->rx_state = RX_TYPE; p->rx_esc = 0; continue; }
        if (p->rx_state == RX_IDLE) continue;
        if (b == ESC) { p->rx_esc = 1; continue; }
        if (p->rx_esc) { b ^= 0x20; p->rx_esc = 0; }
        switch (p->rx_state) {
        case RX_TYPE: p->rx_type = b; p->rx_state = RX_SEQ; break;
        case RX_SEQ:  p->rx_seq = b;  p->rx_state = RX_LEN; break;
        case RX_LEN:
            if (b > HERMES_PACKET_MAX) { p->rx_state = RX_IDLE; break; } /* not ours */
            p->rx_len = b;
            p->rx_got = 0;
            p->rx_state = RX_BODY;
            break;
        case RX_BODY:
            p->rx[p->rx_got++] = b;
            if (p->rx_got == p->rx_len + 2) {
                p->rx_state = RX_IDLE;
                frame_done(p);
            }
            break;
        }
    }
}

/* One user of a link at a time: the program (any thread) or the OS's
 * device tick. take() waits for it; try_take() (the tick) doesn't. */
static int try_take(hermes_packet_state_t *p) {
    hermes_lock();
    int got = !p->busy;
    if (got) p->busy = 1;
    hermes_unlock();
    return got;
}

static void take(hermes_packet_state_t *p) {
    while (!try_take(p)) hermes_thread_yield();
}

static void give(hermes_packet_state_t *p) { p->busy = 0; }

void hermes_packet_init(hermes_packet_state_t *p, int port, long baud) {
    unsigned char *z = (unsigned char *)p;
    for (unsigned i = 0; i < sizeof(*p); i++) z[i] = 0;
    p->port = (signed char)port;
    p->retries = 5;
    /* a try waits for: the longest frame (every byte stuffed) and an ACK,
     * at 10 bits per byte, twice, plus 20 ms for the other side to look */
    unsigned long bytes = 2UL * (HERMES_PACKET_MAX + 5) + 1 + 2UL * 5 + 1;
    unsigned long ms = 20 + 2 * (bytes * 10UL * 1000UL / (unsigned long)(baud > 0 ? baud : 9600));
    p->timeout_ms = (unsigned short)(ms > 65535 ? 65535 : ms);
}

void hermes_packet_pump(hermes_packet_state_t *p) {
    if (!try_take(p)) return; /* the program is using it: it pumps itself */
    pump(p);
    give(p);
}

bool hermes_packet_put(hermes_packet_state_t *p, const unsigned char *b, int n) {
    take(p);
    bool ok = p->tx_len + n <= HERMES_PACKET_MAX;
    if (ok) {
        for (int i = 0; i < n; i++) p->tx[p->tx_len + i] = b[i];
        p->tx_len = (unsigned char)(p->tx_len + n);
    } else {
        p->err = HERMES_PACKET_ERR_TOO_LONG;
        p->tx_len = 0; /* the packet is dropped: build it again */
    }
    give(p);
    return ok;
}

bool hermes_packet_send(hermes_packet_state_t *p) {
    take(p);
    if (p->tx_len == 0) {
        if (p->err != HERMES_PACKET_ERR_TOO_LONG) p->err = HERMES_PACKET_ERR_EMPTY;
        give(p);
        return false;
    }
    p->tx_seq++;
    p->acked = 0;
    bool ok = false;
    for (int attempt = 0; attempt <= p->retries && !ok; attempt++) {
        if (attempt > 0) p->resends++;
        send_frame(p, T_DATA, p->tx_seq, p->tx, p->tx_len);
        uint32_t start = hermes_timer_millis();
        for (;;) {
            pump(p);
            if (p->acked) { ok = true; break; }
            if (hermes_timer_millis() - start >= p->timeout_ms) break;
            hermes_timer_delay_ms(1); /* under the OS the thread sleeps: others run */
        }
    }
    p->err = ok ? HERMES_PACKET_ERR_OK : HERMES_PACKET_ERR_NO_ACK;
    p->tx_len = 0;
    give(p);
    return ok;
}

long hermes_packet_receive(hermes_packet_state_t *p, long wait_ms) {
    take(p);
    p->in_read = 0; /* the previous packet is let go */
    uint32_t start = hermes_timer_millis();
    long n = -1;
    for (;;) {
        pump(p);
        if (p->in_ready) {
            p->in_ready = 0;
            p->in_read = 1;
            n = p->in_len;
            break;
        }
        if (wait_ms <= 0 || (long)(hermes_timer_millis() - start) >= wait_ms) break;
        hermes_timer_delay_ms(1);
    }
    give(p);
    return n;
}

long hermes_packet_get(hermes_packet_state_t *p, long i, int bytes) {
    if (!p->in_read || i < 0 || i + bytes > p->in_len) return -1;
    unsigned long v = 0;
    for (int k = bytes - 1; k >= 0; k--) v = (v << 8) | p->in[i + k];
    return bytes == 4 ? (long)(int32_t)v : (long)v;
}

const char *hermes_packet_text(hermes_packet_state_t *p) {
    if (!p->in_read) return "";
    p->in[p->in_len] = 0;
    return (const char *)p->in;
}

const char *hermes_packet_error(hermes_packet_state_t *p) {
    switch (p->err) {
    case HERMES_PACKET_ERR_NO_ACK: return "no_ack";
    case HERMES_PACKET_ERR_TOO_LONG: return "too_long";
    case HERMES_PACKET_ERR_EMPTY: return "empty";
    default: return "ok";
    }
}

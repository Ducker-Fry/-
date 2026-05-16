#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER   2000
#define ACK_TIMER    300
#define MAX_SEQ      7
#define NR_BUFS      ((MAX_SEQ + 1) / 2)

struct DATA_FRAME {
    unsigned char kind;
    unsigned char seq;
    unsigned char ack;
    unsigned char data[PKT_LEN];
    unsigned int  padding;
};

struct ACK_FRAME {
    unsigned char kind;
    unsigned char ack;
    unsigned int  padding;
};

static unsigned char ack_expected = 0;
static unsigned char next_frame_to_send = 0;
static unsigned char frame_expected = 0;
static unsigned char too_far = NR_BUFS;
static unsigned char nbuffered = 0;

static unsigned char out_buf[NR_BUFS][PKT_LEN];
static unsigned char in_buf[NR_BUFS][PKT_LEN];
static unsigned char arrived[NR_BUFS];
static unsigned char acked[MAX_SEQ + 1];
static int phl_ready = 0;

static void inc(unsigned char *nr)
{
    *nr = (*nr + 1) % (MAX_SEQ + 1);
}

static int between(unsigned char a, unsigned char b, unsigned char c)
{
    return ((a <= b) && (b < c)) ||
           ((c < a) && (a <= b)) ||
           ((b < c) && (c < a));
}

static unsigned char last_frame_received(void)
{
    return (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(unsigned char frame_nr)
{
    struct DATA_FRAME s;
    unsigned char buf_nr = frame_nr % NR_BUFS;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = last_frame_received();
    memcpy(s.data, out_buf[buf_nr], PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
    stop_ack_timer();
}

static void send_ack_frame(unsigned char ack_nr)
{
    struct ACK_FRAME s;

    s.kind = FRAME_ACK;
    s.ack = ack_nr;

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}

static void process_ack(unsigned char ack)
{
    if (between(ack_expected, ack, next_frame_to_send) && !acked[ack]) {
        acked[ack] = 1;
        stop_timer(ack);
    }

    while (nbuffered > 0 && acked[ack_expected]) {
        acked[ack_expected] = 0;
        nbuffered--;
        inc(&ack_expected);
    }
}

static void accept_data_frame(unsigned char seq, unsigned char *data, int len)
{
    unsigned char buf_nr = seq % NR_BUFS;

    if (between(frame_expected, seq, too_far) && !arrived[buf_nr]) {
        arrived[buf_nr] = 1;
        memcpy(in_buf[buf_nr], data, len);
    }

    send_ack_frame(seq);

    while (arrived[frame_expected % NR_BUFS]) {
        unsigned char deliver_nr = frame_expected % NR_BUFS;

        put_packet(in_buf[deliver_nr], PKT_LEN);
        arrived[deliver_nr] = 0;
        inc(&frame_expected);
        inc(&too_far);
    }
}

int main(int argc, char **argv)
{
    int event, arg;
    unsigned char frame[sizeof(struct DATA_FRAME)];
    unsigned char kind, seq, ack;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Selective Repeat protocol, build: " __DATE__"  "__TIME__"\n");

    memset(arrived, 0, sizeof arrived);
    memset(acked, 0, sizeof acked);
    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(out_buf[next_frame_to_send % NR_BUFS]);
            acked[next_frame_to_send] = 0;
            nbuffered++;
            send_data_frame(next_frame_to_send);
            inc(&next_frame_to_send);
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED:
            len = recv_frame(frame, sizeof frame);
            if (len < 5 || crc32(frame, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }

            kind = frame[0];

            if (kind == FRAME_ACK) {
                ack = frame[1];
                dbg_frame("Recv ACK  %d\n", ack);
                process_ack(ack);
            }

            if (kind == FRAME_DATA) {
                seq = frame[1];
                ack = frame[2];
                dbg_frame("Recv DATA %d %d, ID %d\n", seq, ack, *(short *)(frame + 3));

                process_ack(ack);
                accept_data_frame(seq, frame + 3, len - 7);
            }
            break;

        case DATA_TIMEOUT:
            if (between(ack_expected, (unsigned char)arg, next_frame_to_send) &&
                !acked[(unsigned char)arg]) {
                dbg_event("---- DATA %d timeout\n", arg);
                send_data_frame((unsigned char)arg);
            }
            break;

        case ACK_TIMEOUT:
            send_ack_frame(last_frame_received());
            break;
        }

        if (nbuffered < NR_BUFS && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
    }
}

#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER   2000
#define MAX_SEQ      7
#define WINDOW_SIZE  MAX_SEQ

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
static unsigned char nbuffered = 0;
static unsigned char out_buf[MAX_SEQ + 1][PKT_LEN];
static int phl_ready = 0;

static unsigned char last_frame_received(void)
{
    return (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
}

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

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(unsigned char frame_nr)
{
    struct DATA_FRAME s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = last_frame_received();
    memcpy(s.data, out_buf[frame_nr], PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void send_ack_frame(void)
{
    struct ACK_FRAME s;

    s.kind = FRAME_ACK;
    s.ack = last_frame_received();

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}

static void acknowledge_frames(unsigned char ack)
{
    while (nbuffered > 0 && between(ack_expected, ack, next_frame_to_send)) {
        nbuffered--;
        stop_timer(ack_expected);
        inc(&ack_expected);
    }
}

static void retransmit_window(void)
{
    unsigned char frame_nr = ack_expected;
    unsigned char i;

    for (i = 0; i < nbuffered; i++) {
        send_data_frame(frame_nr);
        inc(&frame_nr);
    }
}

int main(int argc, char **argv)
{
    int event, arg;
    unsigned char frame[sizeof(struct DATA_FRAME)];
    unsigned char kind, seq, ack;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Go-Back-N protocol, build: " __DATE__"  "__TIME__"\n");

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(out_buf[next_frame_to_send]);
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
                acknowledge_frames(ack);
            }

            if (kind == FRAME_DATA) {
                seq = frame[1];
                ack = frame[2];
                dbg_frame("Recv DATA %d %d, ID %d\n", seq, ack, *(short *)(frame + 3));

                if (seq == frame_expected) {
                    put_packet(frame + 3, len - 7);
                    inc(&frame_expected);
                }

                acknowledge_frames(ack);
                send_ack_frame();
            }
            break;

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg);
            retransmit_window();
            break;
        }

        if (nbuffered < WINDOW_SIZE && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}

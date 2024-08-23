#include "mongoose.h"
#include <signal.h>

#define PACKET_TYPE_AUTH_DATA 0xAD
#define PACKET_TYPE_GAME_DATA 0xFA
#define PACKET_TYPE_CONNECTED 0xCE
#define PACKET_TYPE_DISCONNECTED 0xDE
#define PACKET_TYPE_ERROR 0xEE

struct PacketHeader {
    uint8_t packet_type;
    uint8_t reserved;
    uint16_t len;
    uint32_t game_id;
    uint32_t sender_id;
    uint32_t receiver_id;
    char *data[0];
} __attribute__((packed));

struct ConState {
    uint64_t recv_time;
    uint32_t game_id;
    uint32_t player_id;
};

#define NAME player_map
#define KEY_TY uint32_t
#define VAL_TY struct mg_connection*
#include "verstable.h"
player_map s_players;

static int s_signo;
// command line arguments
static const char *s_port = "7788";

static void
signal_handler(int signo)
{
    s_signo = signo;
}

static void
send_notification(struct mg_connection *c, char packet_type, struct mg_str resp)
{
    struct PacketHeader pkt = {
        .packet_type = packet_type,
        .len = resp.len,
    };
    mg_send(c, &pkt, sizeof(pkt));
    if (resp.len) {
        mg_send(c, resp.buf, resp.len);
    }
}

static struct mg_connection*
find_player_con(uint32_t player_id)
{
    player_map_itr it = vt_get(&s_players, player_id);
    return vt_is_end(it) ? NULL : it.data->val;
}

static int
handle_packet(struct mg_connection *c, struct ConState *state, struct PacketHeader *pkt)
{
    MG_DEBUG(("PKT %#X len=%u from=%u to=%u", pkt->packet_type, pkt->len, pkt->sender_id, pkt->receiver_id));
    if (!state->player_id) {
        // first packet must'be auth data
        if (pkt->packet_type != PACKET_TYPE_AUTH_DATA) {
            c->is_draining = 1;
            MG_ERROR(("auth requied player_id=%u", pkt->sender_id));
            return -1;
        }
        if (find_player_con(pkt->sender_id)) {
            c->is_closing = 1;
            MG_ERROR(("already connected player_id=%u", pkt->sender_id));
            send_notification(c, PACKET_TYPE_ERROR, mg_str("already connected"));
            return -1;
        }
        state->player_id = pkt->sender_id;
        state->game_id = pkt->game_id;
        vt_insert(&s_players, pkt->sender_id, c);
        return 0;
    }
    if (state->player_id != pkt->sender_id) {
        MG_ERROR(("invalid sender_id=%u", pkt->sender_id));
        return -1;
    }
    if (pkt->packet_type == PACKET_TYPE_GAME_DATA) {
        struct mg_connection *rcon = find_player_con(pkt->receiver_id);
        if(!rcon) {
            MG_ERROR(("remote player %d is disconnected", pkt->receiver_id));
            return 0;
        }
        mg_send(rcon, pkt, sizeof(struct PacketHeader) + pkt->len);
    }
    return 0;
}

static void
proxy_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct ConState *state = (struct ConState*)c->data;
    if (ev == MG_EV_OPEN) {
        c->is_hexdumping = 1;
        state->recv_time = mg_millis();
    } else if (ev == MG_EV_CLOSE) {
        vt_erase(&s_players, state->player_id);
    } else if (ev == MG_EV_POLL) {
        // if (c->is_listening || c->is_closing || c->is_draining) {
        //     return;
        // }
        // if ((state->recv_time + 15000) < mg_millis()) {
        //     MG_ERROR(("recv_time=%u expired"));
        //     c->is_closing = 1;
        // }
    } else if (ev == MG_EV_READ) {
        state->recv_time = mg_millis();
        while (c->recv.len >= sizeof(struct PacketHeader)) {
            struct PacketHeader *pkt = (struct PacketHeader *)c->recv.buf;
            if (pkt->len < (c->recv.len - sizeof(struct PacketHeader)))
                break; // wait for more data
            if (handle_packet(c, state, pkt) < 0)
                break;
            mg_iobuf_del(&c->recv, 0, sizeof(struct PacketHeader) + pkt->len);
        }
    }
    (void)ev_data;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "%s usage:\n"
        "--help                           show help message\n"
        "--port arg                       set the proxy port\n",
        prog);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    mg_log_set(MG_LL_DEBUG);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    if (sizeof(struct PacketHeader) != 16) {
        MG_ERROR(("sizeof PacketHeader != 16, check compiler options"));
        exit(EXIT_FAILURE);
    }
    for (int i = 1; i < argc; i++) {
        if (mg_casecmp("--port", argv[i]) == 0) {
            s_port = argv[++i];
        } else if (mg_casecmp("--help", argv[i]) == 0) {
            usage(argv[0]);
        }
    }
    vt_init(&s_players);
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    char url[100];
    mg_snprintf(url, sizeof(url), "tcp://0.0.0.0:%s", s_port);
    mg_listen(&mgr, url, proxy_fn, NULL);
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 25);
    }
    mg_mgr_free(&mgr);
    return 0;
}

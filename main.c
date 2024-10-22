#include "mongoose.h"
#include <signal.h>

#define PROXY_AUTH_DATA 0xF0
#define PROXY_GAME_DATA 0xF4

#define PROXY_HEADER_LEN 11
struct ProxyHeader {
    uint8_t type;
    uint16_t len;
    uint32_t from_id;
    uint32_t to_id;
    char *data[0];
} __attribute__((packed));

struct ConState {
    uint64_t recv_time;
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

static struct mg_connection*
find_player_con(uint32_t player_id)
{
    player_map_itr it = vt_get(&s_players, player_id);
    return vt_is_end(it) ? NULL : it.data->val;
}

static int
handle_packet(struct mg_connection *c, struct ConState *state, struct ProxyHeader *pkt)
{
    MG_DEBUG(("PKT %#X len=%u from_id=%u to_id=%u player_id=%u", pkt->type, pkt->len, pkt->from_id, pkt->to_id, state->player_id));
    if (!state->player_id) {
        // first packet must'be auth data
        if (pkt->type != PROXY_AUTH_DATA) {
            c->is_draining = 1;
            MG_ERROR(("auth required"));
            return -1;
        }
        if (find_player_con(pkt->from_id)) {
            c->is_closing = 1;
            MG_ERROR(("already connected player_id=%u", pkt->from_id));
            return -1;
        }
        state->player_id = pkt->from_id;
        MG_DEBUG(("player connected player_id=%u", state->player_id));
        vt_insert(&s_players, state->player_id, c);
        pkt->len = 0;
        mg_send(c, pkt, sizeof(struct ProxyHeader));
        return 0;
    }
    if (pkt->type != PROXY_GAME_DATA) {
        MG_ERROR(("invalid proxy header type=%#x player_id=%u", pkt->type, state->player_id));
        return -1;
    }
    struct mg_connection *rcon = find_player_con(pkt->to_id);
    if(!rcon) {
        MG_DEBUG(("ignore, player %d is disconnected", pkt->to_id));
    } else {
        mg_send(rcon, pkt, sizeof(struct ProxyHeader) + pkt->len);
    }
    return 0;
}

static void
proxy_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct ConState *state = (struct ConState*)c->data;
    if (ev == MG_EV_OPEN) {
        //c->is_hexdumping = 1;
        state->recv_time = mg_millis();
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_listening) {
            MG_INFO(("shutdown"));
        } else {
            MG_DEBUG(("player disconnected player_id=%u", state->player_id));
            vt_erase(&s_players, state->player_id);
        }
    } else if (ev == MG_EV_POLL) {
        // if (c->is_listening || c->is_closing || c->is_draining) {
        //     return;
        // }
        // if ((state->recv_time + (60000 * 3)) < mg_millis()) {
        //     MG_ERROR(("recv_time=%u expired", state->recv_time));
        //     c->is_closing = 1;
        // }
    } else if (ev == MG_EV_READ) {
        state->recv_time = mg_millis();
        while (c->recv.len >= PROXY_HEADER_LEN) {
            struct ProxyHeader *pkt = (struct ProxyHeader *)c->recv.buf;
            size_t msg_len = PROXY_HEADER_LEN + pkt->len;
            if (c->recv.len < msg_len)
                break; // wait for more data
            if (handle_packet(c, state, pkt) < 0)
                break;
            mg_iobuf_del(&c->recv, 0, msg_len);
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
    //mg_log_set(MG_LL_DEBUG);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    if (sizeof(struct ProxyHeader) != PROXY_HEADER_LEN) {
        MG_ERROR(("sizeof PacketHeader == %u, expected %u, check compiler options",
            sizeof(struct ProxyHeader), PROXY_HEADER_LEN));
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
        mg_mgr_poll(&mgr, 5);
    }
    mg_mgr_free(&mgr);
    return 0;
}

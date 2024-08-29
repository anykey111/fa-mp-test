#include "mongoose.h"
#include <signal.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define count_of(p) (sizeof(p)/sizeof(p[0]))

enum GameState {
    GAME_STATE_NONE,
    GAME_STATE_IDLE,
    GAME_STATE_LOBBY,
    GAME_STATE_LAUNCHING,
    GAME_STATE_ENDED,
};

#define CMD_STR_MAX_LEN 4096
#define CMD_PARAMS_LIMIT 32

struct GPGNetCmdParam {
    u8 type; // 0 - integer, string otherwise
    u32 val; // hold integer value or str length
    struct mg_str str;
};

struct GPGNetCmd {
    struct mg_str name;
    u32 num_params;
    struct GPGNetCmdParam params[CMD_PARAMS_LIMIT];
};

struct GPGNetClient {
    struct PlayerInfo *player;
    enum GameState game_state;
};

struct ProxyState {
};

struct PlayerInfo {
    u32 is_hosting;
    u32 conbits;
    u32 id;
    u32 lobby_port;
    u32 proxy_port;
    const char *name;
};

struct MPHeader {
    uint8_t  mp_type;
    uint16_t mask;
    uint16_t serial;
    uint16_t serial_ack;
    uint16_t seqno;
    uint16_t seqno_ack;
    uint16_t length;
    uint8_t data[0];
} __attribute__((packed));

static const char *s_port = "7237";

#define FLAG_IS_CONNECTED

static struct PlayerInfo s_players[] = {
    { .is_hosting = 1, .id = 1, .lobby_port = 6123, .proxy_port = 7123, .name = "player1" },
    { .is_hosting = 0, .id = 2, .lobby_port = 6124, .proxy_port = 7124, .name = "player2" }
};

static int s_signo;

static void
signal_handler(int signo)
{
    s_signo = signo;
}

static u32
read_u32(u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static void
pack_u32(u8 *p, u32 u)
{
    p[0] = u & 0xff;
    p[1] = (u >> 8) & 0xff;
    p[2] = (u >> 16) & 0xff;
    p[3] = (u >> 24) & 0xff;
}

static void
send_u32(struct mg_connection *c, u32 u)
{
    u8 buf[4];
    pack_u32(buf, u);
    mg_send(c, buf, sizeof(buf));
}

static void
send_str(struct mg_connection *c, struct mg_str s)
{
    send_u32(c, s.len);
    mg_send(c, s.buf, s.len);
}

static void
send_tagged_str(struct mg_connection *c, struct mg_str s)
{
    mg_send(c, "\x01", 1);
    send_str(c, s);
}

static void
send_tagged_u32(struct mg_connection *c, u32 u)
{
    mg_send(c, "\x00", 1);
    send_u32(c, u);
}

// static struct mg_connection*
// find_player_con(struct mg_mgr *mgr, u32 id)
// {
//     for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next) {
//         struct GPGNetClient *client = (struct GPGNetClient *)c->data;
//         if (client->player && client->player->id == id)
//             return c;
//     }
//     return NULL;
// }

// static void
// proxy_fn(struct mg_connection *c, int ev, void *ev_data)
// {
//     struct ProxyState *proxy = (struct ProxyState *)c->data;
//     if (ev == MG_EV_OPEN)
//         c->is_hexdumping = 1;
//     if (ev != MG_EV_READ)
//         return;
//     (void)ev_data;
// }

static int
handle_command(struct mg_connection *c, struct GPGNetClient *client, struct GPGNetCmd *cmd)
{
    MG_DEBUG(("%.*s", cmd->name.len, cmd->name.buf));
    if (mg_strcmp(cmd->name, mg_str("GameState")) == 0) {
        struct mg_str state = cmd->params[0].str;
        MG_DEBUG(("state=%.*s", state.len, state.buf));
        char url[100];
        if (mg_strcmp(state, mg_str("Idle")) == 0 && client->game_state != GAME_STATE_IDLE) {
            client->game_state = GAME_STATE_IDLE;
            send_str(c, mg_str("CreateLobby"));
            send_u32(c, 5);
            send_tagged_u32(c, 0); // lobby init mode normal
            send_tagged_u32(c, client->player->lobby_port);
            send_tagged_str(c, mg_str(client->player->name));
            send_tagged_u32(c, client->player->id);
            send_tagged_u32(c, 1); // local offer
            //mg_snprintf(url, sizeof(url), "udp://0.0.0.0:%u", client->player->proxy_port);
            // if (!mg_listen(c->mgr, url)) {
            //     MG_ERROR(("port binding failed, url=%s", url));
            //     s_signo = SIGTERM;
            // }
        } else if (mg_strcmp(state, mg_str("Lobby")) == 0 && client->game_state != GAME_STATE_LOBBY) {
            if (client->player->is_hosting) {
                send_str(c, mg_str("HostGame"));
                send_u32(c, 1);
                send_tagged_str(c, mg_str("monument_valley.v0001"));
            } else {
                struct PlayerInfo *host = &s_players[0];
                send_str(c, mg_str("JoinGame"));
                send_u32(c, 3);
                mg_snprintf(url, sizeof(url), "127.0.0.1:%u", host->lobby_port);
                send_tagged_str(c, mg_str(url));
                send_tagged_str(c, mg_str(host->name));
                send_tagged_u32(c, host->id);
            }
        }
    }
    return 0;
}

static void
gpgnet_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct GPGNetClient *client = (struct GPGNetClient *)c->data;
    struct PlayerInfo *player = client->player;
    if (ev == MG_EV_OPEN) {
        c->is_hexdumping = 1;
    } else if (ev == MG_EV_ACCEPT) {
        for (size_t i = 0; i < count_of(s_players); ++i) {
            struct PlayerInfo *player = &s_players[i];
            if (!player->conbits) {
                player->conbits |= 1 << i;
                client->player = player;
                break;
            }
        }
        if (!client->player) {
            MG_ERROR(("no slots availables"));
            c->is_closing = 1;
        }
    } else if (ev == MG_EV_CLOSE && player) {
        if (player->is_hosting) {
            s_signo = SIGTERM;
            MG_INFO(("host is disconnected, closing"));
        }
        for (size_t i = 0; i < count_of(s_players); ++i) {
            struct PlayerInfo *peer = &s_players[i];
            if (player->id != peer->id && (player->conbits & (1 << i))) {
                MG_DEBUG(("disconnect %s from %s", player->name, peer->name));
                send_str(c, mg_str("DisconnectFromPeer"));
                send_u32(c, 3);
                char url[100];
                mg_snprintf(url, sizeof(url), "127.0.0.1:%u", peer->lobby_port);
                send_tagged_str(c, mg_str(url));
                send_tagged_str(c, mg_str(peer->name));
                send_tagged_u32(c, peer->id);
                player->conbits &= ~(1 << i);
                break;
            }
        }
        player->conbits = 0;
    } else if (ev == MG_EV_POLL && player) {
        for (size_t i = 0; i < count_of(s_players); ++i) {
            struct PlayerInfo *peer = &s_players[i];
            if (player->id != peer->id && peer->conbits && !(player->conbits & (1 << i))) {
                MG_DEBUG(("connect %s to %s", player->name, peer->name));
                send_str(c, mg_str("ConnectToPeer"));
                send_u32(c, 3);
                char url[100];
                mg_snprintf(url, sizeof(url), "127.0.0.1:%u", peer->lobby_port);
                send_tagged_str(c, mg_str(url));
                send_tagged_str(c, mg_str(peer->name));
                send_tagged_u32(c, peer->id);
                player->conbits |= 1 << i;
                break;
            }
        }
    } else if (ev == MG_EV_READ) {
        while (c->recv.len > 4) {
            u8 *ptr = c->recv.buf;
            u8 *limit = ptr + c->recv.len;
            u32 len = read_u32(ptr);
            ptr += 4;
            if (len > CMD_STR_MAX_LEN) {
                MG_ERROR(("exceed str length limit %u of %u, offset: %u", len, CMD_STR_MAX_LEN, ptr - c->recv.buf));
                c->is_closing = 1;
                return;
            }
            if (ptr + len > limit)
                return;
            struct GPGNetCmd cmd = {
                .name = mg_str_n((char*)ptr, len)
            };
            ptr += len;
            if (ptr + 4 > limit)
                return;
            cmd.num_params = read_u32(ptr);
            if (cmd.num_params > CMD_PARAMS_LIMIT) {
                MG_ERROR(("exceed parameters limit %u of %u, offset: %u", cmd.num_params, CMD_PARAMS_LIMIT, ptr - c->recv.buf));
                c->is_closing = 1;
                return;
            }
            ptr += 4;
            for (u32 i = 0; i < cmd.num_params; ++i) {
                if (ptr + 5 > limit)
                    return;
                struct GPGNetCmdParam *param = &cmd.params[i];
                param->type = *ptr;
                len = param->val = read_u32(ptr + 1);
                ptr += 5;
                if (param->type) {
                    if (len > CMD_STR_MAX_LEN) {
                        MG_ERROR(("exceed str length limit %u of %u, offset: %u", len, CMD_STR_MAX_LEN, ptr - c->recv.buf));
                        c->is_closing = 1;
                        return;
                    }
                    if (ptr + len > limit)
                        return;
                    param->str = mg_str_n((char*)ptr, len);
                    ptr += len;
                }
            }
            if (handle_command(c, client, &cmd) < 0)
                return;
            mg_iobuf_del(&c->recv, 0, ptr - c->recv.buf);
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
        "--port arg                       set the GPGNet port\n",
        prog);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    mg_log_set(MG_LL_DEBUG);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    for (int i = 1; i < argc; i++) {
        if (mg_casecmp("--port", argv[i]) == 0) {
            s_port = argv[++i];
        } else {
            usage(argv[0]);
        }
    }
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    char url[100];
    mg_snprintf(url, sizeof(url), "tcp://0.0.0.0:%s", s_port);
    mg_listen(&mgr, url, gpgnet_fn, NULL);
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 1000);
    }
    MG_INFO(("exit s_signo=%u", s_signo));
    mg_mgr_free(&mgr);
    return 0;
}

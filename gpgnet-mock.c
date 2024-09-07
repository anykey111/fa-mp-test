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
};


#define MP_CON 0 // Connect Type
#define MP_ANS 1 // Answer Type
#define MP_DAT 4 // Data Type
#define MP_ACK 5 // Acknowledgement Type
#define MP_KPA 6 // KeepAlive Type
#define MP_GBY 7 // Goodbye Type
#define MP_NAT 8 // NAT Type

struct MPHeader {
    uint8_t  type;
    uint32_t mask;
    uint16_t ser;
    uint16_t irt;
    uint16_t seq;
    uint16_t expected;
    uint16_t len;
    uint8_t data[0];
} __attribute__((packed));

struct MPRecentList {
    size_t pos;
    struct MPHeader items[100];
};

struct PlayerInfo {
    u32 conbits;
    u32 id;
    u32 lobby_port;
    u32 proxy_port;
    u16 next_ser;
    const char *name;
    enum GameState game_state;
    struct mg_connection *proxy;
    struct mg_connection *con;
    struct MPRecentList inbox;
};

static int s_fake_ack = 0;
static const char *s_port = "7237";
static FILE *s_log;

#define HOST_ID 1
static struct PlayerInfo s_players[] = {
    { .id = 1, .lobby_port = 6001, .proxy_port = 7001, .name = "player1" },
    { .id = 2, .lobby_port = 6002, .proxy_port = 7002, .name = "player2" },
    { .id = 3, .lobby_port = 6003, .proxy_port = 7003, .name = "player3" }
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

static void
log_packet(u16 rem_port, struct MPHeader *h)
{
    if (!s_log)
        return;
    const char *mp_name =
        h->type == MP_CON ? "CON" :
        h->type == MP_ANS ? "ANS" :
        h->type == MP_DAT ? "DAT" :
        h->type == MP_ACK ? "ACK" :
        h->type == MP_KPA ? "KPA" :
        h->type == MP_GBY ? "GBY" : "UNK";
    static char hex[128000];
    memset(hex, 0, sizeof(hex));
    MG_DEBUG(("%s: %u len\n", mp_name, h->len));
    for (size_t i = 0, n = 0; i<h->len; ++i) {
        n += mg_snprintf(hex + n, sizeof(hex) - n, "%02X", h->data[i]);
    }
    fprintf(s_log, "%u\t%s\t%u\t%u\t%u\t%u\tx'%s'\n", rem_port, mp_name, h->ser, h->irt, h->seq, h->expected, hex);
}

static void
recent_add(struct MPRecentList *list, struct MPHeader *h)
{
    size_t i = (list->pos + 1) % count_of(list->items);
    list->items[i] = *h;
    list->pos = i;
}

static struct MPHeader *
recent_lookup_dat(struct MPRecentList *list, u16 ser)
{
    for (size_t i = 0, limit = 0; i < count_of(list->items) && limit < 10; ++i) {
        struct MPHeader *h = &list->items[i];
        if (h->ser == ser && h->type == MP_DAT) {
            if (h->irt) {
                i = 0;
                limit += 1;
                ser = h->irt;
                continue;
            }
            return h;
        }
    }
    return NULL;
}

static void
proxy_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct PlayerInfo *player = (struct PlayerInfo *)c->fn_data;
    if (ev == MG_EV_OPEN) {
        //c->is_hexdumping = 1;
        MG_DEBUG(("%s proxy is ready", player->name));
    }
    if (ev != MG_EV_READ)
        return;
    uint16_t rem_port = mg_ntohs(c->rem.port);
    struct MPHeader *h = (struct MPHeader *)c->recv.buf;
    if (rem_port < 7000) {
        if (h->ser > player->next_ser)
            player->next_ser = h->ser;
        // packet from the game
        log_packet(rem_port, h);
        struct MPHeader *irt_dat = recent_lookup_dat(&player->inbox, h->irt);
        if (s_fake_ack && h->type == MP_ACK && irt_dat)
            return; // skip ACK only for DAT packets
        if (s_fake_ack && h->type == MP_DAT && irt_dat) {
            struct MPHeader ack = {
                .type = MP_ACK,
                .irt = h->ser,
                .ser = player->next_ser,
                .seq = h->expected,
                .expected = h->seq + 1,
            };
            mg_send(c, &ack, sizeof(ack));
        }
        rem_port += 1000;
    } else {
        // packet from the proxy
        rem_port -= 1000;
        log_packet(rem_port, h);
        recent_add(&player->inbox, h);
    }
    c->rem.port = mg_htons(rem_port);
    MG_DEBUG(("redirect %u to %u", mg_ntohs(c->loc.port), mg_ntohs(c->rem.port)));
    mg_send(c, c->recv.buf, c->recv.len);
    c->recv.len = 0;
    (void)ev_data;
}

static int
handle_command(struct mg_connection *c, struct PlayerInfo *player, struct GPGNetCmd *cmd)
{
    char url[1000];
    int n = 0;
    for (u32 i = 0; i < cmd->num_params; ++i) {
        struct GPGNetCmdParam *p = &cmd->params[i];
        if (p->type) {
            n += mg_snprintf(url + n, sizeof(url) - n, " s:%.*s", p->str.len, p->str.buf);
        } else {
            n += mg_snprintf(url + n, sizeof(url) - n, " u:%d", p->val);
        }
    }
    MG_INFO(("%.*s %s", cmd->name.len, cmd->name.buf, url));
    if (mg_strcmp(cmd->name, mg_str("GameState")) == 0) {
        if (mg_strcmp(cmd->params[0].str, mg_str("Idle")) == 0 && player->game_state != GAME_STATE_IDLE) {
            player->game_state = GAME_STATE_IDLE;
            send_str(c, mg_str("CreateLobby"));
            send_u32(c, 5);
            send_tagged_u32(c, 0); // lobby init mode normal
            send_tagged_u32(c, player->lobby_port);
            send_tagged_str(c, mg_str(player->name));
            send_tagged_u32(c, player->id);
            send_tagged_u32(c, 0); // local offer
            mg_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", player->proxy_port);
            if (!(player->proxy = mg_listen(c->mgr, url, proxy_fn, player))) {
                MG_ERROR(("port binding failed, url=%s", url));
                s_signo = SIGTERM;
            }
        } else if (mg_strcmp(cmd->params[0].str, mg_str("Lobby")) == 0 && player->game_state != GAME_STATE_LOBBY) {
            player->game_state = GAME_STATE_LOBBY;
            if (player->id == 1) {
                send_str(c, mg_str("HostGame"));
                send_u32(c, 1);
                send_tagged_str(c, mg_str("monument_valley.v0001"));
            } else {
                struct PlayerInfo *host = &s_players[0];
                // connect host
                send_str(host->con, mg_str("ConnectToPeer"));
                send_u32(host->con, 3);
                mg_snprintf(url, sizeof(url), "127.0.0.1:%u", player->proxy_port);
                send_tagged_str(host->con, mg_str(url));
                send_tagged_str(host->con, mg_str(player->name));
                send_tagged_u32(host->con, player->id);
                host->conbits |= 1 << player->id;
                // send join
                send_str(c, mg_str("JoinGame"));
                send_u32(c, 3);
                mg_snprintf(url, sizeof(url), "127.0.0.1:%u", host->proxy_port);
                send_tagged_str(c, mg_str(url));
                send_tagged_str(c, mg_str(host->name));
                send_tagged_u32(c, host->id);
                MG_INFO(("JoinGame %s", url));
            }
        }
    } else if (mg_strcmp(cmd->name, mg_str("Connected")) == 0) {
        //u32 id = 0;
        //mg_str_to_num(cmd->params[0].str, 10, &id, sizeof(id));
        //player->conbits |= 1 << id;
    } else if (mg_strcmp(cmd->name, mg_str("Disconnected")) == 0) {
        //u32 id = 0;
        //mg_str_to_num(cmd->params[0].str, 10, &id, sizeof(id));
        //player->conbits &= ~(1 << id);
    }
    return 0;
}

static void
gpgnet_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct GPGNetClient *client = (struct GPGNetClient *)c->data;
    struct PlayerInfo *player = client->player;
    if (ev == MG_EV_OPEN) {
        //c->is_hexdumping = 1;
    } else if (ev == MG_EV_ACCEPT) {
        for (size_t i = 0; i < count_of(s_players); ++i) {
            struct PlayerInfo *player = &s_players[i];
            if (!player->con) {
                player->con = c;
                client->player = player;
                break;
            }
        }
        if (!client->player) {
            MG_ERROR(("no slots availables"));
            c->is_closing = 1;
        }
    } else if (ev == MG_EV_CLOSE && player) {
        if (player->id == 1) {
            s_signo = SIGTERM;
            MG_INFO(("host is disconnected, closing"));
        }
        for (size_t i = 0; i < count_of(s_players); ++i) {
            struct PlayerInfo *peer = &s_players[i];
            if (player->id != peer->id && (player->conbits & (1 << peer->id))) {
                MG_INFO(("disconnect %s from %s", player->name, peer->name));
                send_str(c, mg_str("DisconnectFromPeer"));
                send_u32(c, 3);
                char url[100];
                mg_snprintf(url, sizeof(url), "127.0.0.1:%u", peer->proxy_port);
                send_tagged_str(c, mg_str(url));
                send_tagged_str(c, mg_str(peer->name));
                send_tagged_u32(c, peer->id);
                break;
            }
        }
        player->con = 0;
        player->game_state = GAME_STATE_NONE;
        if (player->proxy)
            player->proxy->is_closing = 1;
    } else if (ev == MG_EV_POLL && player) {
        for (size_t i = 1; i < count_of(s_players); ++i) {
            struct PlayerInfo *peer = &s_players[i];
            // connect every player
            if (player->id == peer->id || player->game_state != GAME_STATE_LOBBY || peer->game_state != GAME_STATE_LOBBY)
                continue;
            if ((player->conbits & (1 << peer->id))) 
                continue;
            MG_INFO(("connect %s to %s", player->name, peer->name));
            send_str(c, mg_str("ConnectToPeer"));
            send_u32(c, 3);
            char url[100];
            mg_snprintf(url, sizeof(url), "127.0.0.1:%u", peer->proxy_port);
            send_tagged_str(c, mg_str(url));
            send_tagged_str(c, mg_str(peer->name));
            send_tagged_u32(c, peer->id);
            player->conbits |= 1 << peer->id;
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
            if (handle_command(c, player, &cmd) < 0)
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
        "--debug                          enable verbose logging\n"
        "--record filename                record all message into .csv file\n"
        "--fake-ack                       send fake MP_ACK for every MP_DAT\n"
        "--port arg                       set the GPGNet port\n",
        prog);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    mg_log_set(MG_LL_INFO);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    for (int i = 1; i < argc; i++) {
        if (mg_casecmp("--port", argv[i]) == 0) {
            s_port = argv[++i];
        } else if (mg_casecmp("--fake-ack", argv[i]) == 0) {
            s_fake_ack = 1;
        } else if (mg_casecmp("--debug", argv[i]) == 0) {
            mg_log_set(MG_LL_DEBUG);
        } else if (mg_casecmp("--record", argv[i]) == 0) {
            const char *filename = argv[++i];
            s_log = fopen(filename, "w+");
            if (!s_log) {
                perror(filename);
                exit(EXIT_FAILURE);
            }
            MG_INFO(("start recording to %s", filename));
            fprintf(s_log, "port\ttype\tser\tirt\tseq\texpected\n");
        } else {
            usage(argv[0]);
        }
    }
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    char url[100];
    mg_snprintf(url, sizeof(url), "tcp://127.0.0.1:%s", s_port);
    mg_listen(&mgr, url, gpgnet_fn, NULL);
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 25);
    }
    MG_INFO(("exit s_signo=%u", s_signo));
    mg_mgr_free(&mgr);
    if (s_log)
        fclose(s_log);
    return 0;
}

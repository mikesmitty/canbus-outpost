/* Host tests for the LCC library: build and run with `make` in this directory. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "lcc/bytes.h"
#include "lcc/datagram.h"
#include "lcc/events.h"
#include "lcc/gridconnect.h"
#include "lcc/memconfig.h"
#include "lcc/node.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

#define NODE_ID   0x050101011234ULL
#define OTHER     0x0AB          /* alias of the "tool" talking to us */

static lcc_frame_t sent[512];
static int nsent;
static int initialized_calls, identify_calls, update_calls, factory_calls, reboot_calls;
static uint32_t now;

static bool hook_send(const lcc_frame_t *f) { if (nsent < 512) sent[nsent++] = *f; return true; }
static void hook_initialized(void) { initialized_calls++; }
static void hook_identify(void) { identify_calls++; }
static void hook_update(void) { update_calls++; }
static void hook_factory(void) { factory_calls++; }
static void hook_reboot(void) { reboot_calls++; }

static char user_name[64] = "Yard throat";
static char user_desc[64] = "East end";

static void setup(void)
{
    nsent = 0;
    initialized_calls = identify_calls = update_calls = factory_calls = reboot_calls = 0;
    now = 1000;
    lcc_node_config_t cfg = {
        .node_id = NODE_ID,
        .manufacturer = "mfg", .model = "model", .hardware_version = "hw",
        .software_version = "sw",
        .user_name = user_name, .user_description = user_desc,
        .hooks = {
            .send = hook_send, .initialized = hook_initialized,
            .identify_requested = hook_identify, .config_update_complete = hook_update,
            .factory_reset = hook_factory, .reboot = hook_reboot,
        },
    };
    lcc_init(&cfg);
}

static void advance(uint32_t ms)
{
    now += ms;
    lcc_run_timers(now);
}

/* Start and claim the alias; leaves the sent log empty. */
static void start_node(void)
{
    lcc_start(now);
    advance(200);
    nsent = 0;
}

static void rx(const lcc_frame_t *f) { lcc_frame_received(f, now); }

static void rx_global(uint16_t mti, uint16_t src, const uint8_t *d, uint8_t len)
{
    lcc_frame_t f = { .id = lcc_message_id(mti, src), .dlc = len };
    if (len) memcpy(f.data, d, len);
    rx(&f);
}

static void rx_addressed(uint16_t mti, uint16_t src, uint16_t dst, const uint8_t *d, uint8_t len)
{
    lcc_frame_t f = { .id = lcc_message_id(mti, src), .dlc = 2 + len };
    f.data[0] = (dst >> 8) & 0x0F;
    f.data[1] = dst & 0xFF;
    if (len) memcpy(f.data + 2, d, len);
    rx(&f);
}

static void rx_datagram(uint16_t src, const uint8_t *d, uint8_t len)
{
    uint16_t dst = lcc_alias();
    if (len <= 8) {
        lcc_frame_t f = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_ONE, dst, src), .dlc = len };
        memcpy(f.data, d, len);
        rx(&f);
        return;
    }
    for (uint8_t off = 0; off < len; off += 8) {
        uint8_t n = len - off > 8 ? 8 : len - off;
        uint8_t type = off == 0 ? LCC_FRAME_DATAGRAM_FIRST :
                       off + n >= len ? LCC_FRAME_DATAGRAM_FINAL : LCC_FRAME_DATAGRAM_MIDDLE;
        lcc_frame_t f = { .id = lcc_datagram_id(type, dst, src), .dlc = n };
        memcpy(f.data, d + off, n);
        rx(&f);
    }
}

static uint16_t mti_of(const lcc_frame_t *f) { return lcc_frame_mti(f->id); }

/* Index of the first sent frame with this MTI from `from`, or -1. */
static int find_mti(uint16_t mti, int from)
{
    for (int i = from; i < nsent; i++)
        if (!lcc_is_control_frame(sent[i].id) &&
            lcc_can_frame_type(sent[i].id) == LCC_FRAME_GLOBAL_ADDRESSED &&
            mti_of(&sent[i]) == mti)
            return i;
    return -1;
}

/* Reassemble the datagram we sent to `dst` starting at frame `from`. */
static int collect_datagram(uint16_t dst, int from, uint8_t *out)
{
    int len = 0;
    for (int i = from; i < nsent; i++) {
        const lcc_frame_t *f = &sent[i];
        if (lcc_is_control_frame(f->id)) continue;
        uint8_t t = lcc_can_frame_type(f->id);
        if (t < LCC_FRAME_DATAGRAM_ONE || t > LCC_FRAME_DATAGRAM_FINAL) continue;
        if (lcc_datagram_dst(f->id) != dst) continue;
        memcpy(out + len, f->data, f->dlc);
        len += f->dlc;
        if (t == LCC_FRAME_DATAGRAM_ONE || t == LCC_FRAME_DATAGRAM_FINAL)
            return len;
    }
    return -1;
}

/* Payload (after the destination alias) of the first addressed reply with this MTI */
static int addressed_payload(uint16_t mti, uint8_t *out)
{
    int len = 0;
    for (int i = 0; i < nsent; i++) {
        if (lcc_is_control_frame(sent[i].id) || mti_of(&sent[i]) != mti) continue;
        memcpy(out + len, sent[i].data + 2, sent[i].dlc - 2);
        len += sent[i].dlc - 2;
        if (!(sent[i].data[0] & LCC_ADDR_NOT_LAST_FRAME))
            return len;
    }
    return -1;
}

static void ack_datagram(uint16_t src)
{
    uint8_t flags = 0;
    rx_addressed(LCC_MTI_DATAGRAM_OK, src, lcc_alias(), &flags, 1);
}

/* ------------------------------------------------------------------ */
/* Node                                                               */
/* ------------------------------------------------------------------ */

static void test_alias_allocation(void)
{
    setup();
    lcc_start(now);
    CHECK(nsent == 4, "expected 4 CID frames, got %d", nsent);
    uint16_t alias = lcc_alias();
    CHECK(alias != 0, "alias is zero");
    for (int i = 0; i < 4 && i < nsent; i++) {
        int seq = 7 - i;
        uint32_t want = lcc_control_id((NODE_ID >> ((seq - 4) * 12)) & 0xFFF, seq, alias);
        CHECK(sent[i].id == want, "CID%d id %08X want %08X", seq, sent[i].id, want);
        CHECK(lcc_is_cid_frame(sent[i].id), "frame %d not recognized as CID", i);
    }
    CHECK(lcc_ms_until_next_timer(now) == 200, "next timer %u", lcc_ms_until_next_timer(now));

    advance(199);
    CHECK(nsent == 4 && !lcc_is_initialized(), "claimed before 200 ms");
    advance(1);
    CHECK(lcc_is_initialized() && initialized_calls == 1, "not initialized after 200 ms");
    CHECK(nsent == 7, "expected RID, AMD, Init Complete; got %d frames", nsent);
    CHECK(sent[4].id == lcc_control_id(LCC_CONTROL_RID, 0, alias), "RID wrong: %08X", sent[4].id);
    CHECK(sent[5].id == lcc_control_id(LCC_CONTROL_AMD, 0, alias) && sent[5].dlc == 6 &&
          lcc_get_be48(sent[5].data) == NODE_ID, "AMD wrong");
    CHECK(mti_of(&sent[6]) == LCC_MTI_INITIALIZATION_COMPLETE &&
          lcc_get_be48(sent[6].data) == NODE_ID, "Init Complete wrong");
    CHECK(lcc_ms_until_next_timer(now) == LCC_NO_TIMER, "timer left armed");
}

static void test_alias_conflict_while_checking(void)
{
    setup();
    lcc_start(now);
    uint16_t first = lcc_alias();
    lcc_frame_t rid = { .id = lcc_control_id(LCC_CONTROL_RID, 0, first) };
    rx(&rid);
    nsent = 0;
    advance(200);
    CHECK(!lcc_is_initialized(), "claimed a contested alias");
    CHECK(nsent == 4 && lcc_is_cid_frame(sent[0].id), "no new CID frames");
    CHECK(lcc_alias() != first && lcc_alias() != 0, "alias not changed");
    advance(200);
    CHECK(lcc_is_initialized(), "second alias not claimed");
}

static void test_alias_defend_and_duplicate(void)
{
    setup();
    start_node();
    uint16_t alias = lcc_alias();

    lcc_frame_t cid = { .id = lcc_control_id(0x123, 7, alias) };
    rx(&cid);
    CHECK(nsent == 1 && sent[0].id == lcc_control_id(LCC_CONTROL_RID, 0, alias), "CID not defended");

    nsent = 0;
    rx_global(LCC_MTI_VERIFY_NODE_ID_GLOBAL, alias, NULL, 0);  /* someone else using our alias */
    CHECK(nsent == 5, "expected AMR + 4 CID, got %d", nsent);
    CHECK(sent[0].id == lcc_control_id(LCC_CONTROL_AMR, 0, alias) &&
          lcc_get_be48(sent[0].data) == NODE_ID, "AMR wrong");
    CHECK(!lcc_is_initialized() && lcc_alias() != alias, "did not restart with a new alias");
    advance(200);
    CHECK(lcc_is_initialized(), "did not reclaim");
}

static void test_ame(void)
{
    setup();
    start_node();
    lcc_frame_t ame = { .id = lcc_control_id(LCC_CONTROL_AME, 0, OTHER) };
    rx(&ame);
    CHECK(nsent == 1 && lcc_control_field(sent[0].id) == LCC_CONTROL_AMD, "global AME not answered");

    nsent = 0;
    ame.dlc = 6;
    lcc_put_be48(ame.data, 0x010203040506ULL);
    rx(&ame);
    CHECK(nsent == 0, "answered AME for another node");
}

static void test_verify_node_id(void)
{
    setup();
    start_node();
    rx_global(LCC_MTI_VERIFY_NODE_ID_GLOBAL, OTHER, NULL, 0);
    CHECK(nsent == 1 && mti_of(&sent[0]) == LCC_MTI_VERIFIED_NODE_ID &&
          lcc_get_be48(sent[0].data) == NODE_ID, "global verify not answered");

    uint8_t other[6], mine[6];
    lcc_put_be48(other, 0x010203040506ULL);
    lcc_put_be48(mine, NODE_ID);
    nsent = 0;
    rx_global(LCC_MTI_VERIFY_NODE_ID_GLOBAL, OTHER, other, 6);
    CHECK(nsent == 0, "answered verify for another node ID");
    rx_global(LCC_MTI_VERIFY_NODE_ID_GLOBAL, OTHER, mine, 6);
    CHECK(nsent == 1, "verify with our ID not answered");

    nsent = 0;
    rx_addressed(LCC_MTI_VERIFY_NODE_ID_ADDRESSED, OTHER, lcc_alias(), NULL, 0);
    CHECK(nsent == 1, "addressed verify not answered");
    rx_addressed(LCC_MTI_VERIFY_NODE_ID_ADDRESSED, OTHER, 0x555, NULL, 0);
    CHECK(nsent == 1, "answered verify addressed to another alias");
    CHECK(identify_calls == 3, "identify hook calls %d", identify_calls);
}

static void test_protocol_support(void)
{
    setup();
    static uint8_t cfg[16];
    static const lcc_memspace_t space = { .space = LCC_SPACE_CDI, .size = 16, .data = cfg };
    lcc_memconfig_add_space(&space);
    start_node();

    rx_addressed(LCC_MTI_PROTOCOL_SUPPORT_INQUIRY, OTHER, lcc_alias(), NULL, 0);
    uint8_t p[16];
    int n = addressed_payload(LCC_MTI_PROTOCOL_SUPPORT_REPLY, p);
    CHECK(n == 6, "PIP reply length %d", n);
    uint64_t bits = lcc_get_be48(p);
    uint64_t want = LCC_PROTOCOL_EVENT_EXCHANGE | LCC_PROTOCOL_SNIP | LCC_PROTOCOL_DATAGRAM |
                    LCC_PROTOCOL_MEMORY_CONFIG | LCC_PROTOCOL_CDI;
    CHECK(bits == want, "PIP bits %012llX want %012llX",
          (unsigned long long)bits, (unsigned long long)want);
    CHECK((lcc_get_be16(sent[0].data) & 0xFFF) == OTHER, "PIP reply not addressed to requester");
}

static void test_snip(void)
{
    setup();
    start_node();
    memset(user_desc, 'x', sizeof(user_desc));  /* not NUL-terminated: capped at 63 */
    rx_addressed(LCC_MTI_SNIP_REQUEST, OTHER, lcc_alias(), NULL, 0);
    memcpy(user_desc, "East end", 9);

    uint8_t p[256];
    int n = addressed_payload(LCC_MTI_SNIP_REPLY, p);
    CHECK(n > 0, "no SNIP reply");
    CHECK(sent[0].data[0] & LCC_ADDR_NOT_LAST_FRAME, "first frame missing more-frames flag");
    CHECK(sent[1].data[0] & LCC_ADDR_NOT_FIRST_FRAME, "second frame missing not-first flag");

    const uint8_t want_head[] = "\x04mfg\0model\0hw\0sw\0\x02Yard throat";
    CHECK(memcmp(p, want_head, sizeof(want_head)) == 0, "SNIP strings wrong");
    int desc_at = (int)sizeof(want_head);
    const char *desc = (const char *)p + desc_at;
    const char *nul = memchr(desc, 0, 200);
    int desc_len = nul ? (int)(nul - desc) : -1;
    CHECK(desc_len == 63 && n == desc_at + 64, "description length %d, total %d", desc_len, n);
}

static void test_optional_interaction_rejected(void)
{
    setup();
    start_node();
    rx_addressed(0x05EB, OTHER, lcc_alias(), NULL, 0);  /* some unsupported addressed MTI */
    uint8_t p[8];
    int n = addressed_payload(LCC_MTI_OPTIONAL_INTERACTION_REJECTED, p);
    CHECK(n == 4 && lcc_get_be16(p) == LCC_ERROR_UNIMPLEMENTED_MTI && lcc_get_be16(p + 2) == 0x05EB,
          "OIR wrong (n=%d)", n);

    nsent = 0;
    rx_addressed(LCC_MTI_OPTIONAL_INTERACTION_REJECTED, OTHER, lcc_alias(), p, 4);
    CHECK(nsent == 0, "answered an OIR");

    lcc_frame_t cont = { .id = lcc_message_id(0x05EB, OTHER), .dlc = 2 };
    cont.data[0] = LCC_ADDR_NOT_FIRST_FRAME | ((lcc_alias() >> 8) & 0xF);
    cont.data[1] = lcc_alias() & 0xFF;
    rx(&cont);
    CHECK(nsent == 0, "rejected a continuation frame");
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

static int consumed;
static void on_event(uintptr_t arg) { consumed += (int)arg; }
static lcc_event_state_t state_valid(uintptr_t arg) { (void)arg; return LCC_EVENT_VALID; }

static void test_events(void)
{
    setup();
    static uint8_t id_a[8], id_b[8], id_zero[8];
    lcc_put_be64(id_a, 0x0501010112340001ULL);
    lcc_put_be64(id_b, 0x0501010112340002ULL);
    static const lcc_event_t consumer = { .id = id_a, .on_event = on_event, .arg = 1 };
    static const lcc_event_t consumer2 = { .id = id_a, .on_event = on_event, .arg = 10 };
    static const lcc_event_t producer = { .id = id_b, .producer = true, .state = state_valid };
    static const lcc_event_t unused = { .id = id_zero, .on_event = on_event, .arg = 100 };
    lcc_event_register(&consumer);
    lcc_event_register(&consumer2);
    lcc_event_register(&producer);
    lcc_event_register(&unused);

    lcc_start(now);
    advance(200);
    CHECK(find_mti(LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN, 0) >= 0 &&
          find_mti(LCC_MTI_PRODUCER_IDENTIFIED_VALID, 0) >= 0, "events not announced at init");
    int announced = 0;
    for (int i = 0; i < nsent; i++)
        if (!lcc_is_control_frame(sent[i].id) && mti_of(&sent[i]) != LCC_MTI_INITIALIZATION_COMPLETE)
            announced++;
    CHECK(announced == 3, "announced %d events, want 3 (zero ID skipped)", announced);

    nsent = 0;
    rx_global(LCC_MTI_EVENTS_IDENTIFY_GLOBAL, OTHER, NULL, 0);
    CHECK(nsent == 3, "identify global: %d frames", nsent);
    nsent = 0;
    rx_addressed(LCC_MTI_EVENTS_IDENTIFY_ADDRESSED, OTHER, lcc_alias(), NULL, 0);
    CHECK(nsent == 3, "identify addressed: %d frames", nsent);
    nsent = 0;
    rx_addressed(LCC_MTI_EVENTS_IDENTIFY_ADDRESSED, OTHER, 0x555, NULL, 0);
    CHECK(nsent == 0, "answered identify for another node");

    consumed = 0;
    rx_global(LCC_MTI_EVENT_REPORT, OTHER, id_a, 8);
    CHECK(consumed == 11, "both consumers should run: consumed=%d", consumed);
    consumed = 0;
    rx_global(LCC_MTI_EVENT_REPORT, OTHER, id_b, 8);
    rx_global(LCC_MTI_EVENT_REPORT, OTHER, id_zero, 8);
    CHECK(consumed == 0, "wrong consumer ran: %d", consumed);

    nsent = 0;
    rx_global(LCC_MTI_PRODUCER_IDENTIFY, OTHER, id_b, 8);
    CHECK(nsent == 1 && mti_of(&sent[0]) == LCC_MTI_PRODUCER_IDENTIFIED_VALID &&
          memcmp(sent[0].data, id_b, 8) == 0, "producer identify wrong");
    nsent = 0;
    rx_global(LCC_MTI_CONSUMER_IDENTIFY, OTHER, id_a, 8);
    CHECK(nsent == 2 && mti_of(&sent[0]) == LCC_MTI_CONSUMER_IDENTIFIED_UNKNOWN, "consumer identify wrong");
    nsent = 0;
    rx_global(LCC_MTI_PRODUCER_IDENTIFY, OTHER, id_a, 8);
    CHECK(nsent == 0, "consumer answered a producer identify");

    nsent = 0;
    lcc_event_send(id_b);
    CHECK(nsent == 1 && mti_of(&sent[0]) == LCC_MTI_EVENT_REPORT &&
          memcmp(sent[0].data, id_b, 8) == 0, "event report wrong");
    nsent = 0;
    lcc_event_send(id_zero);
    CHECK(nsent == 0, "sent an all-zero event");
}

/* ------------------------------------------------------------------ */
/* Datagrams and memory configuration                                 */
/* ------------------------------------------------------------------ */

static const char cdi[] = "<?xml version='1.0'?><cdi><identification><manufacturer>m"
                          "</manufacturer></identification></cdi>";
static uint8_t config[32];
static uint32_t written_addr, written_len;
static int written_calls;
static void on_written(uint32_t a, uint32_t l) { written_addr = a; written_len = l; written_calls++; }

static const lcc_memspace_t cdi_space = {
    .space = LCC_SPACE_CDI, .size = sizeof(cdi), .data = (const uint8_t *)cdi,
};
static const lcc_memspace_t config_space = {
    .space = LCC_SPACE_CONFIG, .size = sizeof(config), .data = config, .writable = config,
    .written = on_written,
};

static void setup_memconfig(void)
{
    setup();
    memset(config, 0, sizeof(config));
    written_calls = 0;
    lcc_memconfig_add_space(&cdi_space);
    lcc_memconfig_add_space(&config_space);
    start_node();
}

/* Send a datagram, check the OK flags, return the reply datagram (acked). */
static int request(const uint8_t *d, uint8_t len, bool expect_reply, uint8_t *reply)
{
    nsent = 0;
    rx_datagram(OTHER, d, len);
    uint8_t p[8];
    int n = addressed_payload(LCC_MTI_DATAGRAM_OK, p);
    if (n < 1) {
        int r = addressed_payload(LCC_MTI_DATAGRAM_REJECTED, p);
        return r == 2 ? -(int)lcc_get_be16(p) : -1;
    }
    bool pending = (p[0] & 0x80) != 0;
    CHECK(pending == expect_reply, "reply-pending flag %d, want %d", pending, expect_reply);
    if (!expect_reply)
        return 0;
    int rlen = collect_datagram(OTHER, 0, reply);
    if (rlen > 0)
        ack_datagram(OTHER);
    return rlen;
}

static void test_memconfig_space_info_and_options(void)
{
    setup_memconfig();
    uint8_t r[80];

    const uint8_t info[] = { 0x20, 0x84, 0xFD };
    int n = request(info, sizeof(info), true, r);
    const uint8_t want[] = { 0x20, 0x87, 0xFD, 0, 0, 0, 31, 0x00 };
    CHECK(n == 8 && memcmp(r, want, 8) == 0, "config space info wrong (n=%d)", n);

    const uint8_t info_cdi[] = { 0x20, 0x84, 0xFF };
    n = request(info_cdi, sizeof(info_cdi), true, r);
    CHECK(n == 8 && r[1] == 0x87 && lcc_get_be32(r + 3) == sizeof(cdi) - 1 && r[7] == 0x01,
          "CDI space info wrong");

    const uint8_t info_none[] = { 0x20, 0x84, 0x10 };
    n = request(info_none, sizeof(info_none), true, r);
    CHECK(n == 3 && r[1] == 0x86 && r[2] == 0x10, "unknown space info wrong");

    const uint8_t opts[] = { 0x20, 0x80 };
    n = request(opts, sizeof(opts), true, r);
    const uint8_t want_opts[] = { 0x20, 0x82, 0x60, 0x00, 0xF2, 0xFF, 0xFD };
    CHECK(n == 7 && memcmp(r, want_opts, 7) == 0, "options reply wrong");
}

static void test_memconfig_read_write(void)
{
    setup_memconfig();
    uint8_t r[80];

    /* Write 0x4000 (16384) as two bytes at address 4: stored exactly as sent */
    const uint8_t wr[] = { 0x20, 0x01, 0, 0, 0, 4, 0x40, 0x00 };
    CHECK(request(wr, sizeof(wr), false, r) == 0, "write not acknowledged");
    CHECK(config[4] == 0x40 && config[5] == 0x00, "write stored %02X %02X", config[4], config[5]);
    CHECK(written_calls == 1 && written_addr == 4 && written_len == 2, "written hook wrong");

    const uint8_t rd[] = { 0x20, 0x41, 0, 0, 0, 4, 2 };
    int n = request(rd, sizeof(rd), true, r);
    const uint8_t want[] = { 0x20, 0x51, 0, 0, 0, 4, 0x40, 0x00 };
    CHECK(n == 8 && memcmp(r, want, 8) == 0, "read back wrong (n=%d)", n);

    /* Multi-frame write with the space given explicitly */
    uint8_t big[7 + 20] = { 0x20, 0x00, 0, 0, 0, 8, 0xFD };
    for (int i = 0; i < 20; i++) big[7 + i] = (uint8_t)(0xA0 + i);
    CHECK(request(big, sizeof(big), false, r) == 0, "multi-frame write rejected");
    CHECK(config[8] == 0xA0 && config[27] == 0xB3, "multi-frame write data wrong");

    /* Read the CDI in 64-byte chunks */
    const uint8_t rd_cdi[] = { 0x20, 0x43, 0, 0, 0, 0, 64 };
    n = request(rd_cdi, sizeof(rd_cdi), true, r);
    CHECK(n == 6 + 64 && memcmp(r + 6, cdi, 64) == 0, "CDI read wrong (n=%d)", n);
    const uint8_t rd_tail[] = { 0x20, 0x43, 0, 0, 0, 64, 64 };
    n = request(rd_tail, sizeof(rd_tail), true, r);
    CHECK(n == 6 + (int)sizeof(cdi) - 64 && r[n - 1] == 0, "CDI tail read wrong (n=%d)", n);

    /* Errors */
    const uint8_t rd_oob[] = { 0x20, 0x41, 0, 0, 0, 32, 4 };
    n = request(rd_oob, sizeof(rd_oob), true, r);
    CHECK(n == 8 && r[1] == 0x59 && lcc_get_be16(r + 6) == 0x1082, "out-of-bounds read reply wrong");
    const uint8_t rd_space[] = { 0x20, 0x40, 0, 0, 0, 0, 0x10, 4 };
    n = request(rd_space, sizeof(rd_space), true, r);
    CHECK(n == 9 && r[1] == 0x58 && r[6] == 0x10 && lcc_get_be16(r + 7) == 0x1081,
          "unknown-space read reply wrong");
    const uint8_t wr_cdi[] = { 0x20, 0x03, 0, 0, 0, 0, 0x41 };
    CHECK(request(wr_cdi, sizeof(wr_cdi), false, r) == -0x1083, "write to CDI not rejected");
    const uint8_t wr_oob[] = { 0x20, 0x01, 0, 0, 0, 31, 1, 2 };
    CHECK(request(wr_oob, sizeof(wr_oob), false, r) == -0x1082, "write past end not rejected");
    const uint8_t bad_cmd[] = { 0x20, 0xC0 };
    CHECK(request(bad_cmd, sizeof(bad_cmd), false, r) == -LCC_ERROR_UNIMPLEMENTED_SUBCMD,
          "unknown command not rejected");
    const uint8_t bad_type[] = { 0x30, 0x01 };
    CHECK(request(bad_type, sizeof(bad_type), false, r) == -LCC_ERROR_UNIMPLEMENTED_CMD,
          "unknown datagram type not rejected");
}

static void test_memconfig_commands(void)
{
    setup_memconfig();
    uint8_t r[80];

    uint8_t lock[8] = { 0x20, 0x88 };
    lcc_put_be48(lock + 2, 0x020304050607ULL);
    int n = request(lock, sizeof(lock), true, r);
    CHECK(n == 8 && r[1] == 0x8A && lcc_get_be48(r + 2) == 0x020304050607ULL, "lock reply wrong");
    lcc_put_be48(lock + 2, 0x0A0B0C0D0E0FULL);
    n = request(lock, sizeof(lock), true, r);
    CHECK(n == 8 && lcc_get_be48(r + 2) == 0x020304050607ULL, "lock taken by a second node");

    const uint8_t done[] = { 0x20, 0xA8 };
    CHECK(request(done, sizeof(done), false, r) == 0 && update_calls == 1, "update complete");

    const uint8_t reboot[] = { 0x20, 0xA9 };
    CHECK(request(reboot, sizeof(reboot), false, r) == 0, "reboot not acknowledged");
    CHECK(reboot_calls == 0, "rebooted before replies could go out");
    advance(100);
    CHECK(reboot_calls == 1, "did not reboot");

    uint8_t reset[8] = { 0x20, 0xAA };
    lcc_put_be48(reset + 2, 0x010203040506ULL);
    CHECK(request(reset, sizeof(reset), false, r) == -LCC_ERROR_INVALID_ARGS && factory_calls == 0,
          "factory reset with wrong node ID accepted");
    lcc_put_be48(reset + 2, NODE_ID);
    CHECK(request(reset, sizeof(reset), false, r) == 0 && factory_calls == 1, "factory reset");
    advance(100);
    CHECK(reboot_calls == 2, "no reboot after factory reset");
}

static void test_datagram_receive_edges(void)
{
    setup_memconfig();
    uint16_t me = lcc_alias();
    uint8_t p[8];

    /* Second sender while one is mid-datagram: refused at its final frame */
    lcc_frame_t a1 = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_FIRST, me, 0x111), .dlc = 8,
                       .data = { 0x20, 0x00, 0, 0, 0, 0, 0xFD, 1 } };
    lcc_frame_t b1 = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_FIRST, me, 0x222), .dlc = 8,
                       .data = { 0x20, 0x00, 0, 0, 0, 0, 0xFD, 9 } };
    lcc_frame_t b2 = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_FINAL, me, 0x222), .dlc = 1, .data = { 9 } };
    lcc_frame_t a2 = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_FINAL, me, 0x111), .dlc = 1, .data = { 2 } };
    nsent = 0;
    rx(&a1); rx(&b1); rx(&b2);
    CHECK(addressed_payload(LCC_MTI_DATAGRAM_REJECTED, p) == 2 &&
          lcc_get_be16(p) == LCC_ERROR_BUFFER_UNAVAILABLE &&
          (lcc_get_be16(sent[0].data) & 0xFFF) == 0x222, "busy sender not refused");
    nsent = 0;
    rx(&a2);
    CHECK(addressed_payload(LCC_MTI_DATAGRAM_OK, p) == 1 && config[0] == 1 && config[1] == 2,
          "first sender's datagram lost");

    /* Final frame with no first frame */
    nsent = 0;
    lcc_frame_t stray = { .id = lcc_datagram_id(LCC_FRAME_DATAGRAM_FINAL, me, 0x333), .dlc = 1 };
    rx(&stray);
    CHECK(addressed_payload(LCC_MTI_DATAGRAM_REJECTED, p) == 2 &&
          lcc_get_be16(p) == LCC_ERROR_OUT_OF_ORDER, "stray final not rejected");

    /* An abandoned datagram times out */
    rx(&a1);
    CHECK(lcc_ms_until_next_timer(now) == 3000, "no reassembly timeout armed");
    advance(3000);
    nsent = 0;
    rx(&a2);
    CHECK(addressed_payload(LCC_MTI_DATAGRAM_REJECTED, p) == 2, "stale reassembly not dropped");
}

static void test_datagram_send_retries(void)
{
    setup_memconfig();
    const uint8_t info[] = { 0x20, 0x84, 0xFD };
    uint8_t first[80], again[80], code[2];

    nsent = 0;
    rx_datagram(OTHER, info, sizeof(info));
    int n = collect_datagram(OTHER, 0, first);
    CHECK(n == 8, "no reply datagram");

    /* Temporary rejection: resent after a delay */
    lcc_put_be16(code, LCC_ERROR_BUFFER_UNAVAILABLE);
    nsent = 0;
    rx_addressed(LCC_MTI_DATAGRAM_REJECTED, OTHER, lcc_alias(), code, 2);
    CHECK(nsent == 0, "resent immediately");
    advance(200);
    CHECK(collect_datagram(OTHER, 0, again) == n && memcmp(first, again, n) == 0, "not resent");
    ack_datagram(OTHER);
    CHECK(lcc_ms_until_next_timer(now) == LCC_NO_TIMER, "timer armed after OK");

    /* Permanent rejection: dropped */
    nsent = 0;
    rx_datagram(OTHER, info, sizeof(info));
    lcc_put_be16(code, LCC_ERROR_PERMANENT);
    rx_addressed(LCC_MTI_DATAGRAM_REJECTED, OTHER, lcc_alias(), code, 2);
    nsent = 0;
    advance(3000);
    CHECK(nsent == 0, "resent after permanent rejection");

    /* No answer at all: dropped after a timeout, then the next one goes out */
    const uint8_t opts[] = { 0x20, 0x80 };
    nsent = 0;
    rx_datagram(OTHER, info, sizeof(info));
    rx_datagram(OTHER, opts, sizeof(opts));
    int replies = 0;
    for (int i = 0; i < nsent; i++)
        if (lcc_can_frame_type(sent[i].id) == LCC_FRAME_DATAGRAM_ONE) replies++;
    CHECK(replies == 1, "second reply sent before the first was acknowledged");
    nsent = 0;
    advance(3000);
    CHECK(collect_datagram(OTHER, 0, again) == 7 && again[1] == 0x82, "queued reply not sent after timeout");
}

/* ------------------------------------------------------------------ */
/* GridConnect                                                        */
/* ------------------------------------------------------------------ */

static void test_gridconnect(void)
{
    lcc_frame_t f = { .id = 0x195B4ABC, .dlc = 3, .data = { 0x01, 0xAB, 0xFF } };
    char buf[LCC_GC_MAX_LEN];
    int n = lcc_gc_encode(&f, buf, sizeof(buf));
    CHECK(n == 19 && strcmp(buf, ":X195B4ABCN01ABFF;\n") == 0, "encode: '%s'", buf);

    lcc_gc_parser_t p = {0};
    lcc_frame_t out;
    bool got = false;
    for (const char *c = ":x195b4abcn01abff;"; *c; c++)
        got = lcc_gc_parse(&p, *c, &out);
    CHECK(got && out.id == f.id && out.dlc == 3 && memcmp(out.data, f.data, 3) == 0, "parse");

    got = false;
    for (const char *c = ":X195B4ABCN0;"; *c; c++)
        got = lcc_gc_parse(&p, *c, &out);
    CHECK(!got, "accepted an odd number of data digits");

    got = false;
    for (const char *c = "junk:X10701ABCN;"; *c; c++)
        got = lcc_gc_parse(&p, *c, &out);
    CHECK(got && out.id == 0x10701ABC && out.dlc == 0 && lcc_gc_idle(&p), "resync after junk");
}

int main(void)
{
    test_alias_allocation();
    test_alias_conflict_while_checking();
    test_alias_defend_and_duplicate();
    test_ame();
    test_verify_node_id();
    test_protocol_support();
    test_snip();
    test_optional_interaction_rejected();
    test_events();
    test_memconfig_space_info_and_options();
    test_memconfig_read_write();
    test_memconfig_commands();
    test_datagram_receive_edges();
    test_datagram_send_retries();
    test_gridconnect();

    printf("lcc: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}

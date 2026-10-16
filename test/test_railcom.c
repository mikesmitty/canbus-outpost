// Drives the real railcom.c decoder with synthetic cutouts: CUTOUT_SIGNAL
// timing at the S-9.3.2 limits, bit-rate error, collisions and glitches.
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
static int verbose;
void dbg_printf(const char *fmt, ...) { if (!verbose) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

#include "../src/railcom.c"
uint64_t mock_now_us;
pio_hw_t mock_pio0;
mock_dma_hw_t mock_dma;

static int ndata, nrep[4]; static railcom_data_t last[4]; static int last_ch = -1;
void railcom_on_data(int ch, railcom_data_t *d) { ndata++; nrep[ch]++; last[ch] = *d; last_ch = ch; }
static int sig_state = -1;
void railcom_on_signal(int ch, bool a) { if (ch == 0) sig_state = a; }

// ---- synthesis -------------------------------------------------------------
static uint8_t enc[64];
typedef struct { int blk; int orient_b; double t_us; uint8_t code; } tx_t;
typedef struct {
    double tcs, tce, d_off, d_on;   // µs after end bit
    double skew;                    // bit-rate error, e.g. 0.02
    int load[4];                    // DCC load current on block (0/1)
    tx_t tx[32]; int ntx;
} cut_t;

static uint16_t sample_at(const cut_t *c, double t)  // t: µs after end bit
{
    uint16_t s = 0;
    bool dcc_on = (t < c->tcs) || (t >= c->tce);
    // CUTOUT_SIGNAL as seen through the opto
    if (t >= c->tcs + c->d_off && t < c->tce + c->d_on) s |= CUTOUT_MASK;
    if (dcc_on) {
        // DCC "1" half-bits of 58 µs; polarity alternates
        bool pos = ((long)floor(t / 58.0)) % 2 == 0;
        for (int b = 0; b < 4; b++)
            if (c->load[b]) s |= pos ? blk_a_mask[b] : blk_b_mask[b];
        return s;
    }
    double bit_us = 4.0 * (1.0 + c->skew);
    for (int i = 0; i < c->ntx; i++) {
        const tx_t *x = &c->tx[i];
        double rel = t - x->t_us;
        if (rel < 0 || rel >= 10 * bit_us) continue;
        int bit = (int)(rel / bit_us);
        bool space;
        if (bit == 0) space = true;
        else if (bit == 9) space = false;
        else space = !((x->code >> (bit - 1)) & 1);
        if (space) s |= x->orient_b ? blk_b_mask[x->blk] : blk_a_mask[x->blk];
    }
    return s;
}

static uint64_t abs_sample = 100000;   // running sample counter (time base)

static void run_cutout(const cut_t *c, int latency_us)
{
    uint64_t E = abs_sample;   // sample index of the end-bit zero crossing
    double t_end = c->tce + c->d_on + latency_us;
    for (long k = -1600; k < (long)(t_end * SAMPLES_PER_US) + 2; k++) {
        double t = k / (double)SAMPLES_PER_US;
        ring[(E + k) & RING_MASK] = sample_at(c, t);
    }
    uint64_t newest = E + (uint64_t)(t_end * SAMPLES_PER_US);
    newest &= ~1ull;   // DMA writes whole words (2 samples)
    mock_dma.ch[0].write_addr = (uintptr_t)&ring[newest & RING_MASK];
    mock_now_us = newest / SAMPLES_PER_US;
    stats.edges++;
    process_cutout();
    housekeeping();
    abs_sample = E + 12000;   // next packet ~6 ms later
}

static void add_dg(cut_t *c, int blk, int ob, double t0, int id, int data)
{
    int v0 = (id << 2) | (data >> 6), v1 = data & 0x3F;
    c->tx[c->ntx++] = (tx_t){ blk, ob, t0, enc[v0] };
    c->tx[c->ntx++] = (tx_t){ blk, ob, t0 + 40.0 * (1.0 + c->skew), enc[v1] };
}

static cut_t base(void) { cut_t c = { .tcs = 29, .tce = 471, .d_off = 30, .d_on = 4 }; return c; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Sends ADR1/ADR2 alternately for n cutouts on block blk.
static void send_address(cut_t proto, int blk, int ob, int hi, int lo, int n, double t1)
{
    for (int i = 0; i < n; i++) {
        cut_t c = proto;
        if (i % 2 == 0) add_dg(&c, blk, ob, t1, 1, hi);
        else            add_dg(&c, blk, ob, t1, 2, lo);
        run_cutout(&c, 20);
    }
}

static void reset(void)
{
    memset(blocks, 0, sizeof(blocks));
    memset(&stats, 0, sizeof(stats));
    stats.high_min_us = UINT32_MAX;
    signal_active = false;  // each case starts as if just powered up
    last_cutout_us = 0;
    ndata = 0; last_ch = -1;
    memset(nrep, 0, sizeof(nrep));
    memset(last, 0, sizeof(last));
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    for (int i = 0; i < 256; i++) if (decode_table[i] < 64) enc[decode_table[i]] = (uint8_t)i;
    railcom_init();

    // 1. Short address 3, block 1, orientation A, nominal timing, load current on all blocks
    reset();
    { cut_t p = base(); for (int b = 0; b < 4; b++) p.load[b] = 1;
      send_address(p, 0, 0, 0x00, 3, 4, 80); }
    CHECK(ndata == 1 && last_ch == 0, "short addr: ndata=%d ch=%d", ndata, last_ch);
    CHECK(last[0].occupied && last[0].address == 3 && last[0].addr_type == RAILCOM_ADDR_SHORT &&
          last[0].orientation == RAILCOM_ORIENT_A, "short addr: got %u type %d orient %d",
          last[0].address, last[0].addr_type, last[0].orientation);
    CHECK(stats.cutouts == 4 && stats.framing[0] == 0, "cutouts=%u framing=%u", stats.cutouts, stats.framing[0]);
    CHECK(sig_state == 1, "signal not active");

    // 2. Long address 1234 on block 3, orientation B, earliest cutout end (TCE 454) and start
    reset();
    { cut_t p = base(); p.tcs = 26; p.tce = 454; p.d_on = 2; p.d_off = 10; p.load[2] = 1;
      send_address(p, 2, 1, 0x80 | (1234 >> 8), 1234 & 0xFF, 4, 80); }
    CHECK(ndata == 1 && last[2].address == 1234 && last[2].addr_type == RAILCOM_ADDR_LONG &&
          last[2].orientation == RAILCOM_ORIENT_B, "long/early: ndata=%d addr=%u type=%d or=%d",
          ndata, last[2].address, last[2].addr_type, last[2].orientation);

    // 3. Latest cutout end (TCE 488), slow opto turn-off, channel 1 at its latest (137 µs second byte)
    reset();
    { cut_t p = base(); p.tcs = 32; p.tce = 488; p.d_on = 6; p.d_off = 120; p.load[1] = 1;
      send_address(p, 1, 0, 0x60, 0x80 | 25, 4, 97); }
    CHECK(ndata == 1 && last[1].address == 25 && last[1].addr_type == RAILCOM_ADDR_CONSIST,
          "consist/late: ndata=%d addr=%u type=%d ch1ok=%u bad=%u short=%u long=%u",
          ndata, last[1].address, last[1].addr_type, stats.ch1_ok[1], stats.ch1_bad[1],
          stats.short_high, stats.long_high);

    // 4. Bit-rate skew ±2% with channel 2 traffic from another decoder in the same block
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        reset();
        cut_t p = base(); p.skew = 0.02 * sgn; p.load[3] = 1;
        for (int i = 0; i < 4; i++) {
            cut_t c = p;
            if (i % 2 == 0) add_dg(&c, 3, 0, 80, 1, 0x00); else add_dg(&c, 3, 0, 80, 2, 99);
            add_dg(&c, 3, 1, 193, 0, 0x55);                  // ch2 POM reply, other orientation
            c.tx[c.ntx++] = (tx_t){ 3, 1, 273, 0x0F };       // ch2 ACK
            run_cutout(&c, 20);
        }
        CHECK(ndata == 1 && last[3].address == 99 && last[3].orientation == RAILCOM_ORIENT_A,
              "skew %+d%%: ndata=%d addr=%u or=%d framing=%u ch2=%u", 2 * sgn, ndata,
              last[3].address, last[3].orientation, stats.framing[3], stats.ch2_bytes[3]);
        CHECK(stats.ch2_bytes[3] == 12, "skew %+d%%: ch2 bytes=%u", 2 * sgn, stats.ch2_bytes[3]);
    }

    // 5. Collision: two decoders in block 2 sending different addresses in channel 1
    reset();
    { cut_t p = base(); p.load[1] = 1;
      for (int i = 0; i < 6; i++) {
          cut_t c = p;
          if (i % 2 == 0) { add_dg(&c, 1, 0, 80, 1, 0x00); add_dg(&c, 1, 0, 80, 1, 0x00); }
          else            { add_dg(&c, 1, 0, 80, 2, 3);    add_dg(&c, 1, 0, 80, 2, 44); }
          run_cutout(&c, 20);
      } }
    CHECK(ndata == 0, "collision reported an address: %u", last[1].address);

    // 6. Glitch on CUTOUT_SIGNAL (short high) is rejected, wake with no edge is harmless
    reset();
    { cut_t c = base(); c.tcs = 0; c.tce = 50; c.d_off = 0; c.d_on = 0; run_cutout(&c, 20); }
    CHECK(stats.short_high == 1 && stats.cutouts == 0, "glitch: short=%u cutouts=%u", stats.short_high, stats.cutouts);

    // 7. Loco leaves: cutouts continue without channel 1 data -> vacated after
    // 1 s; the empty blocks are reported vacant once at the same time
    reset();
    { cut_t p = base(); p.load[0] = 1; send_address(p, 0, 0, 0x00, 7, 4, 80);
      CHECK(ndata == 1, "vacate: early reports: ndata=%d", ndata);
      cut_t empty = base();
      for (int i = 0; i < 200; i++) run_cutout(&empty, 20);   // 200 x 6 ms = 1.2 s
      CHECK(nrep[0] == 2 && !last[0].occupied && last[0].address == 7,
            "vacate: reports=%d occ=%d addr=%u", nrep[0], last[0].occupied, last[0].address);
      for (int b = 1; b < 4; b++)
          CHECK(nrep[b] == 1 && !last[b].occupied && last[b].address == 0,
                "vacant block %d: reports=%d occ=%d addr=%u", b + 1, nrep[b],
                last[b].occupied, last[b].address); }

    // 8. Cutouts stop -> signal lost
    mock_now_us += 600000; housekeeping();
    CHECK(sig_state == 0, "signal still active");

    // 9. Signal lost while occupied: the block holds its report until cutouts
    // resume, then stays occupied if the loco is seen again and is vacated
    // a second later if not
    reset();
    { cut_t p = base(); p.load[0] = 1; send_address(p, 0, 0, 0x00, 9, 4, 80);
      send_address(p, 1, 0, 0x00, 10, 4, 80);
      mock_now_us += 5000000; housekeeping();                 // power off for 5 s
      CHECK(sig_state == 0 && ndata == 2 && last[0].occupied && last[1].occupied,
            "power off: sig=%d ndata=%d occ=%d/%d", sig_state, ndata,
            last[0].occupied, last[1].occupied);
      abs_sample += (uint64_t)5000000 * SAMPLES_PER_US;
      send_address(p, 0, 0, 0x00, 9, 4, 80);                   // block 1 loco still there
      CHECK(sig_state == 1 && ndata == 2 && last[1].occupied,
            "power back: reported too early: ndata=%d", ndata);
      send_address(p, 0, 0, 0x00, 9, 200, 80);
      CHECK(nrep[0] == 1 && last[0].occupied, "power back: block 1 reports=%d occ=%d",
            nrep[0], last[0].occupied);
      CHECK(nrep[1] == 2 && !last[1].occupied && last[1].address == 10,
            "power back: block 2 reports=%d occ=%d addr=%u", nrep[1], last[1].occupied,
            last[1].address); }

    // 10. Timing sweep: every TCE/TCS/opto combination at the spec limits, ch1 at 80 and 97 µs
    int sweep_fail = 0, sweep_n = 0;
    double tces[] = {454, 471, 488}, tcss[] = {26, 32}, dons[] = {2, 6}, doffs[] = {5, 150}, t1s[] = {80, 97};
    for (int a = 0; a < 3; a++) for (int b = 0; b < 2; b++) for (int d = 0; d < 2; d++)
    for (int e = 0; e < 2; e++) for (int f = 0; f < 2; f++) for (int g = 0; g < 2; g++) {
        reset();
        cut_t p = base(); p.tce = tces[a]; p.tcs = tcss[b]; p.d_on = dons[d]; p.d_off = doffs[e];
        p.load[0] = g;
        send_address(p, 0, 0, 0x00, 42, 4, t1s[f]);
        sweep_n++;
        if (!(ndata == 1 && last[0].address == 42)) {
            sweep_fail++;
            printf("  sweep fail: tce=%g tcs=%g don=%g doff=%g t1=%g load=%d ndata=%d ch1ok=%u bad=%u short=%u\n",
                   tces[a], tcss[b], dons[d], doffs[e], t1s[f], g, ndata, stats.ch1_ok[0], stats.ch1_bad[0], stats.short_high);
        }
    }
    CHECK(sweep_fail == 0, "timing sweep: %d of %d failed", sweep_fail, sweep_n);

    // 11. Channel 2 at its latest (ends at 454 µs) with the earliest cutout end must decode cleanly
    reset();
    { cut_t c = base(); c.tce = 454; c.d_on = 2;
      for (int k = 0; k < 6; k++) c.tx[c.ntx++] = (tx_t){ 0, 0, 454 - 40.0 * (6 - k), enc[k + 1] };
      run_cutout(&c, 20); }
    CHECK(stats.ch2_bytes[0] == 6 && stats.framing[0] == 0, "ch2 late: bytes=%u framing=%u",
          stats.ch2_bytes[0], stats.framing[0]);

    printf("%s (%d failures)\n", fails ? "FAILED" : "all tests passed", fails);
    return fails != 0;
}

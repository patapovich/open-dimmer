/* PC tests of the real src/phase.c (the zero-cross ISR glue) against fake
 * registers (tests/fake/fake_regs.h): make test. Built once per variant
 * (normal, SIM_ZC, DIAG_REPORT, MEASURE).
 *
 * Mains safety properties checked on 50/60 Hz edge trains with jitter,
 * asymmetric detectors, noise pulses, late ISRs and lost edges:
 *   - every TIM1 update event (UG) with a non-zero compare lands at a real
 *     zero-cross edge (within the tracker's window; under random noise the
 *     window plus ZC_BOUNCE_TICKS, 0.1 ms), and only while the tracker is
 *     locked;
 *   - the compare preloads are 0 after every ISR (a wrap loads gates-off);
 *   - every compare is below the reload (the gate turns off within the period);
 *   - a lost lock leaves the channels dark until the fade-in after a relock.
 * Not covered, because timing alone cannot tell them from the mains: a noise
 * train at a fixed phase from power-up, or noise inside the window in
 * consecutive half cycles (each moves the reference by up to a window).
 * phase.c is compiled with its fader setters renamed (Makefile FADER_HOOKS):
 * the hooks here check that the main loop changes a fader with interrupts off. */
#define _DEFAULT_SOURCE
#include "fake_regs.h"
#include "board.h"
#include "curves.h"
#include "dim_core.h"
#include "phase.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SIM_ZC
#define SIM_ZC 0
#endif
#ifndef MEASURE
#define MEASURE 0
#endif
#ifndef DIAG_REPORT
#define DIAG_REPORT 0
#endif

#define ZC_MASK   (1u << 4)
#define POLL_TICKS 50                       /* main loop phase_poll() every 1 ms */

TIM_TypeDef  fake_tim1, fake_tim16;
EXTI_TypeDef fake_exti;
GPIO_TypeDef fake_gpioa, fake_gpiob;
fake_egr_t   fake_egr_log[FAKE_EGR_SLOTS];
unsigned     fake_egr_writes;
uint8_t      fake_nvic_prio[FAKE_IRQ_COUNT];
uint8_t      fake_nvic_enabled[FAKE_IRQ_COUNT];
uint8_t      fake_irq_off;
uint32_t     fake_apb1_grp2_clocks;
int          fake_exti4_port = -1;

unsigned fake_egr_slot(void) {
    unsigned s = fake_egr_writes++ % FAKE_EGR_SLOTS;
    fake_egr_log[s].tim1  = (fake_tim_snap_t){ fake_tim1.ARR, fake_tim1.CCR3, fake_tim1.CCR4, fake_tim1.CNT };
    fake_egr_log[s].tim16 = (fake_tim_snap_t){ fake_tim16.ARR, fake_tim16.CCR3, fake_tim16.CCR4, fake_tim16.CNT };
    fake_tim1.egr[s]      = 0;
    fake_tim16.egr[s]     = 0;
    return s;
}

void EXTI4_15_IRQHandler(void);
#if SIM_ZC
void TIM16_IRQHandler(void);
#endif

static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_failed++;                                                          \
        }                                                                        \
    } while (0)

/* ---- fader hooks: main-loop fader changes must be made with IRQs off ---- */

static int g_in_isr;
static int g_fader_changes, g_fader_changes_irq_on;

static void fader_change(void) {
    if (!g_in_isr) {
        g_fader_changes++;
        g_fader_changes_irq_on += !fake_irq_off;
    }
}
void checked_dim_fader_set(dim_fader_t *f, uint8_t on, uint16_t level) {
    fader_change();
    dim_fader_set(f, on, level);
}
void checked_dim_fader_set_floor(dim_fader_t *f, uint16_t floor) {
    fader_change();
    dim_fader_set_floor(f, floor);
}
void checked_dim_fader_set_profile(dim_fader_t *f, const dim_profile_t *p) {
    fader_change();
    dim_fader_set_profile(f, p);
}
void checked_dim_fader_hold_start(dim_fader_t *f, int8_t dir, uint16_t lo, uint16_t hi) {
    fader_change();
    dim_fader_hold_start(f, dir, lo, hi);
}
void checked_dim_fader_hold_start_from_dark(dim_fader_t *f, uint16_t lo, uint16_t hi) {
    fader_change();
    dim_fader_hold_start_from_dark(f, lo, hi);
}
uint16_t checked_dim_fader_hold_stop(dim_fader_t *f) {
    fader_change();
    return dim_fader_hold_stop(f);
}
void checked_dim_fader_hold_cancel(dim_fader_t *f) {
    fader_change();
    dim_fader_hold_cancel(f);
}

#if !SIM_ZC
/* ---- deterministic random numbers ---- */

static uint32_t g_rng = 1;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int rnd_in(int lo, int hi) {                 /* lo..hi inclusive */
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

typedef struct {
    long t;              /* 20 us ticks */
    int  real;           /* a real zero-cross edge of the detector */
    int  delay;          /* its ISR runs this much later (a flash stall) */
} edge_t;
#endif

/* ---- the simulated module ---- */

#define MAX_EDGES 40000
#if !SIM_ZC
static edge_t g_edges[MAX_EDGES];
static int    g_n_edges;
#endif
static long   g_real[MAX_EDGES];             /* real edges, in order */
static int    g_n_real;

typedef struct {
    int  ug_on;          /* UGs with a non-zero compare */
    int  ug_on_far;      /* ... more than tol ticks from a real edge */
    long worst;          /* largest distance of such a UG from a real edge */
    int  preload_bad;    /* ISRs that left a compare preload non-zero */
    int  arr_bad;        /* UGs with compare >= reload */
    int  on_unlocked;    /* UGs with a compare while the tracker was not locked */
    int  isr;            /* ISRs run */
    int  lit_after_loss; /* lock lost with a channel left lit */
    long first_on;       /* time of the first UG with a compare, -1 = none */
    long last_ug;        /* last TIM1 UG */
    fake_tim_snap_t last; /* TIM1 at that UG */
} stats_t;

static stats_t g_st;
static int     g_far_now;        /* this ISR turned a gate on away from a real edge */
static long    g_now;
static long    g_tol;            /* allowed distance of a UG with a compare from a real edge */
static uint8_t g_pa4 = 1;        /* detector output level (pull-up: idle high) */

static void set_now(long t) {
    g_now          = t;
    fake_tim16.CNT = (uint32_t)(t & 0xFFFF);
}

static long dist_to_real(long t) {
    /* distance to the nearest real edge (binary search) */
    int lo = 0, hi = g_n_real;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_real[mid] < t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    long d = 1L << 30;
    if (lo < g_n_real) {
        d = g_real[lo] - t;
    }
    if (lo > 0 && t - g_real[lo - 1] < d) {
        d = t - g_real[lo - 1];
    }
    return d;
}

static void reset_stats(void) {
    memset(&g_st, 0, sizeof g_st);
    g_st.first_on = -1;
    g_st.last_ug  = -1;
}

/* PHASE_TEST_TRACE=1: the ISRs before a turn-on away from a real edge */
#define TRACE_N 12
static struct {
    long     t, d;
    uint32_t acted;
    uint8_t  locked;
    uint16_t hp;
} g_trace[TRACE_N];
static unsigned g_trace_n;

static void trace_isr(void) {
    unsigned i        = g_trace_n++ % TRACE_N;
    g_trace[i].t      = g_now;
    g_trace[i].d      = dist_to_real(g_now);
    g_trace[i].acted  = g_phase_dbg.zc_edges;
    g_trace[i].locked = g_phase_dbg.locked;
    g_trace[i].hp     = g_phase_dbg.half_period;
}

static void trace_dump(void) {
    if (!getenv("PHASE_TEST_TRACE")) {
        return;
    }
    printf("  turn-on %ld ticks from a real edge; the ISRs before it:\n", dist_to_real(g_now));
    for (unsigned k = g_trace_n > TRACE_N ? g_trace_n - TRACE_N : 0; k < g_trace_n; k++) {
        unsigned i = k % TRACE_N;
        printf("    t %7ld  from real %4ld  acted %u  locked %u  hp %u\n", g_trace[i].t, g_trace[i].d,
               (unsigned)g_trace[i].acted, g_trace[i].locked, g_trace[i].hp);
    }
}

/* Check the TIM1 update events of one ISR (EGR slots from..fake_egr_writes). */
static void check_ugs(unsigned from) {
    for (unsigned n = from; n != fake_egr_writes; n++) {
        unsigned s = n % FAKE_EGR_SLOTS;
        if (!(fake_tim1.egr[s] & TIM_EGR_UG)) {
            continue;                                   /* a TIM16 update */
        }
        fake_tim_snap_t u = fake_egr_log[s].tim1;
        uint32_t        c = u.ccr3 > u.ccr4 ? u.ccr3 : u.ccr4;
        g_st.last_ug      = g_now;
        g_st.last         = u;
        if (c >= u.arr) {
            g_st.arr_bad++;
        }
        if (c) {
            long d = dist_to_real(g_now);
            g_st.ug_on++;
            if (g_st.first_on < 0) {
                g_st.first_on = g_now;
            }
            if (d > g_st.worst) {
                g_st.worst = d;
            }
            if (d > g_tol) {
                g_st.ug_on_far++;
                g_far_now = 1;
            }
            if (!g_phase_dbg.locked) {
                g_st.on_unlocked++;
            }
        }
    }
}

static void after_isr(unsigned from) {
    check_ugs(from);
    if (fake_tim1.CCR3 || fake_tim1.CCR4) {
        g_st.preload_bad++;
    }
    g_st.isr++;
    trace_isr();
    if (g_far_now) {
        g_far_now = 0;
        trace_dump();
    }
}

/* A lock loss in one ISR or poll: the counter changed. Read at its own width,
 * whatever that is (it passes 255 in the long noisy runs). */
static int lock_lost_since(uint32_t before) {
    return (uint32_t)g_phase_dbg.lock_losses != before;
}

static void run_exti(long t) {
    set_now(t);
    unsigned from   = fake_egr_writes;
    uint32_t losses = g_phase_dbg.lock_losses;
    fake_exti.PR |= ZC_MASK;
    g_in_isr = 1;
    EXTI4_15_IRQHandler();
    g_in_isr     = 0;
    fake_exti.PR = 0;
    after_isr(from);
    if (lock_lost_since(losses) && (phase_current(0) || phase_current(1))) {
        g_st.lit_after_loss++;
    }
}

static void poll(long t) {
    set_now(t);
    uint32_t losses = g_phase_dbg.lock_losses;
    phase_poll();
    if (lock_lost_since(losses) && (phase_current(0) || phase_current(1))) {
        g_st.lit_after_loss++;
    }
}

#if !SIM_ZC
/* Gate of channel ch (0 = CH4, 1 = CH3) at time t as TIM1 drives it: on from
 * the last UG until its compare; after the wrap the preload (left by the ISR)
 * applies. */
static int gate_on(int ch, long t) {
    if (g_st.last_ug < 0 || (fake_tim1.BDTR & TIM_BDTR_MOE) == 0) {
        return 0;
    }
    long     dt2 = (t - g_st.last_ug) * 10;      /* 2 us ticks */
    uint32_t c   = ch ? g_st.last.ccr3 : g_st.last.ccr4;
    if (dt2 >= (long)g_st.last.arr) {
        c   = ch ? fake_tim1.CCR3 : fake_tim1.CCR4;   /* loaded at the wrap */
        dt2 = dt2 % ((long)g_st.last.arr + 1);
    }
    return dt2 < (long)c;
}

/* ---- edge trains ---- */

typedef struct {
    int      hz;            /* 50 or 60 */
    int      asym;          /* the two kinds of half cycle: hp + asym and hp - asym */
    int      jitter;        /* +- ticks on every real edge */
    int      halves;        /* half cycles */
    int      noise_pct;     /* % of half cycles with a noise pulse ... */
    int      noise_from;    /* ... from this half cycle on */
    int      noise_dense;   /* 0: never in two half cycles in a row */
    int      noise_lo;      /* the pulse this many ticks after the real edge ... */
    int      noise_hi;      /* ... up to this many, but before the next real edge */
    int      noise_width;   /* the pulse: an edge and its return this much later */
    int      delay_pct;     /* % of real edges whose ISR runs late (never two in a row) ... */
    int      delay_max;     /* ... by 1..this many ticks */
    int      drop_pct;      /* % of real edges lost (never two in a row) */
    uint32_t seed;
} train_t;

static void make_train(const train_t *tr) {
    g_rng     = tr->seed ? tr->seed : 1;
    g_n_edges = 0;
    g_n_real  = 0;
    double hp = tr->hz == 60 ? 50000.0 / 120.0 : 500.0;
    long   t0 = 1000;
    int    noisy = 0, late = 0, dropped = 0;
    for (int k = 0; k < tr->halves && g_n_edges + 3 < MAX_EDGES; k++) {
        long t = t0 + (long)(k * hp + 0.5) + ((k & 1) ? tr->asym : 0);
        if (tr->jitter) {
            t += rnd_in(-tr->jitter, tr->jitter);
        }
        g_real[g_n_real++] = t;
    }
    for (int k = 0; k < g_n_real; k++) {
        long t = g_real[k];
        int  d = !late && tr->delay_pct && rnd_in(1, 100) <= tr->delay_pct ? rnd_in(1, tr->delay_max) : 0;
        late   = d != 0;
        if (!dropped && tr->drop_pct && rnd_in(1, 100) <= tr->drop_pct) {
            dropped = 1;
        } else {
            dropped                = 0;
            g_edges[g_n_edges++] = (edge_t){ t, 1, d };
        }
        int may = k >= tr->noise_from && k + 1 < g_n_real && (tr->noise_dense || !noisy);
        noisy   = may && tr->noise_pct && rnd_in(1, 100) <= tr->noise_pct;
        if (noisy) {
            long hi = t + tr->noise_hi;
            if (hi > g_real[k + 1] - 1) {
                hi = g_real[k + 1] - 1;
            }
            long n = t + tr->noise_lo + (long)(rnd() % (uint32_t)(hi - t - tr->noise_lo + 1));
            g_edges[g_n_edges++] = (edge_t){ n, 0, 0 };
            g_edges[g_n_edges++] = (edge_t){ n + tr->noise_width, 0, 0 };
        }
    }
    /* a pulse can end after the next real edge: sort (insertion, nearly sorted) */
    for (int i = 1; i < g_n_edges; i++) {
        edge_t e = g_edges[i];
        int    j = i - 1;
        while (j >= 0 && g_edges[j].t > e.t) {
            g_edges[j + 1] = g_edges[j];
            j--;
        }
        g_edges[j + 1] = e;
    }
}

/* Feed the edges in time order with main-loop polls in between. A late ISR
 * runs at edge + delay; edges before it only keep the EXTI pending. */
static void feed(long until) {
    long next_poll = g_now + POLL_TICKS;
    long pending   = -1;                             /* ISR time of a pending EXTI */
    for (int i = 0; i <= g_n_edges; i++) {
        long te = i < g_n_edges ? g_edges[i].t : until;
        while (1) {
            long tnext = pending >= 0 && pending <= te ? pending : te;
            while (next_poll <= tnext) {
                poll(next_poll);
                next_poll += POLL_TICKS;
            }
            if (pending >= 0 && pending <= te) {
                run_exti(pending);
                pending = -1;
                continue;
            }
            break;
        }
        if (i == g_n_edges) {
            break;
        }
        g_pa4          = (uint8_t)!g_pa4;
        fake_gpioa.IDR = g_pa4 ? (fake_gpioa.IDR | ZC_MASK) : (fake_gpioa.IDR & ~ZC_MASK);
        if (pending < 0) {
            pending = te + g_edges[i].delay;
        }
    }
    while (next_poll <= until) {
        poll(next_poll);
        next_poll += POLL_TICKS;
    }
}

#endif /* !SIM_ZC */

static void module_start(void) {
    set_now(0);
    fake_gpioa.IDR = ZC_MASK;
    g_pa4          = 1;
    phase_init();
    for (uint8_t ch = 0; ch < 2; ch++) {
        dim_profile_t p;
        dim_profile_default(&p);
        phase_set_profile(ch, &p);
        phase_set_range(ch, 10, 1000);
        phase_set_curve(ch, 0);
    }
}

/* Each scenario runs in its own process: phase.c's state starts fresh. */
static int run_isolated(void (*fn)(void), const char *name) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        g_failed = 0;
        fn();
        fflush(stdout);
        _exit(g_failed > 100 ? 100 : g_failed);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int failed = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (failed) {
        printf("  in %s\n", name);
    }
    return failed;
}

#if !SIM_ZC
/* ---- normal, DIAG and MEASURE builds ---- */

static void print_stats(const char *name) {
    if (getenv("PHASE_TEST_VERBOSE")) {
        printf("  %-56s isr %5d on %5d far %d worst %3ld lost-lit %d\n", name, g_st.isr, g_st.ug_on,
               g_st.ug_on_far, g_st.worst, g_st.lit_after_loss);
    }
}

/* Checked after every run. */
static void check_invariants(void) {
    CHECK(g_st.preload_bad == 0);
    CHECK(g_st.arr_bad == 0);
    CHECK(g_st.on_unlocked == 0);
    CHECK(g_st.ug_on_far == 0);
    CHECK(g_st.lit_after_loss == 0);
}

static void run_train(const char *name, const train_t *tr, uint8_t curve, uint16_t level, long tol) {
    module_start();
    for (uint8_t ch = 0; ch < 2; ch++) {
        phase_set_curve(ch, curve);
        phase_set(ch, 1, ch ? (uint16_t)(level / 2 + 10) : level);
    }
    reset_stats();
    g_tol = tol;
    make_train(tr);
    feed(g_real[g_n_real - 1] + 400);                    /* before the lock times out */
    print_stats(name);
    check_invariants();
}

/* Distance allowed between a turn-on and a real edge, in ticks: the window
 * (an edge that close to where one is due cannot be told from the real one,
 * 0.42 ms at 50 Hz, ~40 V at 230 V), plus the jitter of the real edges and the
 * width of a noise pulse. */
static long tol_for(int hz, int jitter, int width) {
    return (hz == 60 ? 417 : 500) / ZC_WINDOW_DIV + 2 * jitter + width + 1;
}

/* Under random noise the window plus ZC_BOUNCE_TICKS (0.5 ms at 50 Hz, ~50 V
 * at 230 V). A pulse less than 0.1 ms before the real edge cannot be told from
 * it (the real edge then looks like its bounce) and can shift the timing by
 * that much, at the lock-up or later. A pulse further inside the window moves
 * nothing: the real edge right after it gives it away. (Before that, noise
 * pulled the two kinds of half cycle apart and this was twice the window,
 * while locked too; the first tracker turned the gates on mid cycle here, up
 * to ~250 V.) A late ISR moves the two kinds apart by a quarter of its delay
 * for a few periods; with the late ISRs of test_everything_at_once the runs
 * stay inside this bound. The noise in these trains is never in two half
 * cycles in a row: in consecutive half cycles, noise inside the window moves
 * the edge taken by up to a window each time. */
static long tol_noise(int hz, int jitter, int width) {
    return tol_for(hz, jitter, width) + ZC_BOUNCE_TICKS;
}

static void test_clean_50_and_60_hz_lock_and_drive(void) {
    for (int hz = 50; hz <= 60; hz += 10) {
        for (uint8_t curve = 0; curve < CURVE_COUNT; curve++) {
            train_t tr = { .hz = hz, .jitter = 1, .halves = 600, .seed = 11u + curve };
            run_train("clean", &tr, curve, 1000, 0);
            CHECK(g_st.ug_on == 596);
            /* the first turn-on at the 5th edge: 4 good half cycles */
            CHECK(g_st.first_on == g_real[4]);
            CHECK(g_phase_dbg.lock_losses == 0);
        }
    }
}

static void test_asymmetric_detector_locks_and_follows(void) {
    /* PA4 high and low last differently long (on the spare 10.08 / 10.02 ms,
     * an asymmetry of 1.5 ticks): up to 8 % apart is followed */
    const int asym[] = { 2, 8, 14 };
    for (int hz = 50; hz <= 60; hz += 10) {
        for (unsigned i = 0; i < sizeof asym / sizeof asym[0]; i++) {
            train_t tr = { .hz = hz, .asym = asym[i], .jitter = 1, .halves = 600, .seed = 5 };
            run_train("asymmetric", &tr, 1, 800, 0);
            CHECK(g_st.ug_on == 596);
            CHECK(g_st.first_on == g_real[4]);
            CHECK(g_phase_dbg.lock_losses == 0);
        }
    }
}

static void test_half_cycles_more_unequal_than_8_percent_never_lock(void) {
    /* 9.2 / 10.8 ms: not a mains detector, or noise taking every other edge */
    train_t tr = { .hz = 50, .asym = 40, .halves = 400 };
    run_train("unequal", &tr, 0, 1000, 0);
    CHECK(g_st.ug_on == 0);
}

static void test_noise_on_the_lock_completing_edge(void) {
    /* A 40 us pulse anywhere in the half cycle after the 4th real edge: it
     * must not complete the lock (the old tracker turned the gates on at it,
     * ~190 V at +8 ms). Only its return edge right where the real edge is due
     * may count, within the window. */
    for (int off = ZC_HALF_MIN_TICKS; off <= 497; off++) {
        train_t tr = { .hz = 50, .halves = 40 };
        module_start();
        phase_set(0, 1, 1000);
        reset_stats();
        g_tol = tol_for(50, 0, 2);
        make_train(&tr);
        g_edges[g_n_edges++] = (edge_t){ g_real[3] + off, 0, 0 };
        g_edges[g_n_edges++] = (edge_t){ g_real[3] + off + 2, 0, 0 };
        for (int i = g_n_edges - 2; i < g_n_edges; i++) {    /* keep the order */
            for (int j = i; j > 0 && g_edges[j - 1].t > g_edges[j].t; j--) {
                edge_t e       = g_edges[j];
                g_edges[j]     = g_edges[j - 1];
                g_edges[j - 1] = e;
            }
        }
        feed(g_real[g_n_real - 1] + 100);
        if (g_st.ug_on_far || g_st.preload_bad || g_st.on_unlocked) {
            printf("  noise at +%d on the lock-completing edge\n", off);
        }
        check_invariants();
        CHECK(g_st.ug_on >= 30);
        if (off + 2 < 500 - 500 / ZC_WINDOW_DIV) {
            CHECK(g_st.first_on == g_real[4]);               /* the pulse changed nothing */
        }
    }
}

static void test_sparse_noise_turns_on_only_at_real_edges(void) {
    /* 40 us pulses anywhere from 7.14 ms after a real edge to the next one
     * (shorter ones are under ZC_HALF_MIN_TICKS), in up to every other half
     * cycle, from power-up on; both mains frequencies, symmetric or not */
    const int pct[] = { 2, 10, 30, 100 };
    for (int hz = 50; hz <= 60; hz += 10) {
        for (unsigned i = 0; i < sizeof pct / sizeof pct[0]; i++) {
            for (int asym = 0; asym <= 8; asym += 8) {
                for (uint32_t seed = 1; seed <= 4; seed++) {
                    train_t tr = { .hz = hz, .asym = asym, .jitter = 1, .halves = 2000, .noise_pct = pct[i],
                                   .noise_lo = ZC_HALF_MIN_TICKS, .noise_hi = 600, .noise_width = 2,
                                   .seed = seed * 1000u + (uint32_t)i * 7u + (uint32_t)asym };
                    run_train("sparse noise", &tr, 2, 1000, tol_noise(hz, 1, 2));
                    CHECK(g_st.ug_on > 2000 * (100 - pct[i]) / 100 / 2);   /* still mostly lit */
                }
            }
        }
    }
}

static void test_dense_noise_away_from_the_edge_is_ignored_once_locked(void) {
    /* a pulse in every half cycle, more than the window before the real edge:
     * once locked, none counts and the light stays on */
    for (int hz = 50; hz <= 60; hz += 10) {
        int     hp = hz == 60 ? 417 : 500;
        train_t tr = { .hz = hz, .jitter = 1, .halves = 2000, .noise_pct = 100, .noise_from = 10,
                       .noise_dense = 1, .noise_lo = ZC_HALF_MIN_TICKS,
                       .noise_hi = hp - hp / ZC_WINDOW_DIV - 4, .noise_width = 2, .seed = 77 };
        run_train("dense noise, locked", &tr, 4, 1000, 1);
        CHECK(g_st.ug_on == 1996);
        CHECK(g_phase_dbg.lock_losses == 0);
    }
}

static void test_fixed_phase_train_near_the_edge_does_not_capture_the_lock(void) {
    /* a pulse every half cycle at the same phase, 0.5..0.9 ms before the real
     * edge (inside the old +-8 % window: it captured the lock at ~70 V) */
    for (int before = 25; before <= 45; before += 5) {
        train_t tr = { .hz = 50, .jitter = 1, .halves = 1000 };
        module_start();
        phase_set(0, 1, 1000);
        reset_stats();
        g_tol = 1;
        make_train(&tr);
        for (int k = 50; k + 1 < g_n_real && g_n_edges + 2 < MAX_EDGES; k++) {
            g_edges[g_n_edges++] = (edge_t){ g_real[k + 1] - before, 0, 0 };
            g_edges[g_n_edges++] = (edge_t){ g_real[k + 1] - before + 2, 0, 0 };
        }
        for (int i = 1; i < g_n_edges; i++) {
            for (int j = i; j > 0 && g_edges[j - 1].t > g_edges[j].t; j--) {
                edge_t e       = g_edges[j];
                g_edges[j]     = g_edges[j - 1];
                g_edges[j - 1] = e;
            }
        }
        feed(g_real[g_n_real - 1] + 400);
        check_invariants();
        CHECK(g_st.ug_on == 996);
        CHECK(g_phase_dbg.lock_losses == 0);
    }
}

static void test_late_isrs_and_lost_edges(void) {
    /* ISRs late by up to 2 ms (a flash stall), edges lost: a gate turns on
     * at most the window after a real edge, never mid cycle, and a lost lock
     * leaves the channels dark */
    for (int hz = 50; hz <= 60; hz += 10) {
        for (int delay = 5; delay <= 100; delay += 19) {
            train_t tr = { .hz = hz, .jitter = 1, .halves = 2000, .delay_pct = 5, .delay_max = delay,
                           .drop_pct = 1, .seed = 900u + (uint32_t)delay };
            run_train("late ISRs + lost edges", &tr, 3, 900, tol_for(hz, 1, 0));
            CHECK(g_st.ug_on > 1200);
            CHECK(g_phase_dbg.lock_losses > 0);
        }
    }
}

static void test_everything_at_once(void) {
    for (uint32_t seed = 1; seed <= 24; seed++) {
        int     hz = seed & 1 ? 50 : 60;
        train_t tr = { .hz = hz, .asym = (int)(seed % 3) * 4, .jitter = 1, .halves = 3000,
                       .noise_pct = 20, .noise_lo = ZC_HALF_MIN_TICKS, .noise_hi = 600, .noise_width = 3,
                       .delay_pct = 3, .delay_max = 60, .drop_pct = 1, .seed = seed * 7919u };
        run_train("noise + late ISRs + lost edges + asymmetry", &tr, (uint8_t)(seed % CURVE_COUNT), 1000,
                  tol_noise(hz, 1, 3));
        CHECK(g_st.ug_on > 2000);
    }
}

static void test_mains_stops_gates_go_dark(void) {
    train_t tr = { .hz = 50, .halves = 100 };
    module_start();
    phase_set(0, 1, 1000);
    phase_set(1, 1, 1000);
    reset_stats();
    g_tol = 0;
    make_train(&tr);
    long last = g_real[g_n_real - 1];
    feed(last);
#if !MEASURE
    CHECK(gate_on(0, last + 1) && gate_on(1, last + 1));     /* full: lit right after the edge */
#endif
    CHECK(fake_tim1.CCR3 == 0 && fake_tim1.CCR4 == 0);
    /* no more edges: dark by the end of the reload period, and the lock goes */
    long wrap = last + (long)(g_st.last.arr / 10) + 1;
    for (long t = wrap; t < wrap + 3000; t += 7) {
        CHECK(!gate_on(0, t) && !gate_on(1, t));
    }
    g_n_edges = 0;
    feed(last + 2000);                                  /* only polls now */
    CHECK(!g_phase_dbg.locked);
    CHECK(g_phase_dbg.lock_losses == 1);
    CHECK(phase_current(0) == 0 && phase_current(1) == 0);
    CHECK(g_st.lit_after_loss == 0);
}

static void test_reload_above_every_compare(void) {
    /* the whole accepted half-period range, at full duty */
    for (uint16_t hp = ZC_HALF_MIN_TICKS; hp <= ZC_HALF_MAX_TICKS; hp++) {
        CHECK(dim_compare(hp, DIM_DUTY_MAX) < dim_reload(hp));
        CHECK(dim_reload(hp) > hp * 10u);                    /* the wrap comes after the next edge */
    }
}

static void test_zero_cross_input_and_priorities(void) {
    module_start();
    CHECK(fake_gpioa.mode[4] == LL_GPIO_MODE_INPUT && fake_gpioa.pull[4] == LL_GPIO_PULL_UP);
    CHECK(fake_exti4_port == (int)LL_EXTI_CONFIG_PORTA);
    CHECK((fake_exti.RTSR & ZC_MASK) && (fake_exti.FTSR & ZC_MASK) && (fake_exti.IMR & ZC_MASK));
    CHECK(fake_nvic_enabled[EXTI4_15_IRQn]);
    CHECK(fake_nvic_prio[EXTI4_15_IRQn] == IRQ_PRIO_ZC);
    /* the UART (one receive register, 86.8 us per byte) above the zero cross
     * (up to ~90 us), both above the 1 ms tick */
    CHECK(IRQ_PRIO_UART < IRQ_PRIO_ZC && IRQ_PRIO_ZC < IRQ_PRIO_TICK);
#if MEASURE
    /* the measurement build never hands the gates to the timer */
    CHECK(fake_gpioa.mode[0] != LL_GPIO_MODE_ALTERNATE && fake_gpioa.mode[1] != LL_GPIO_MODE_ALTERNATE);
#else
    CHECK(fake_gpioa.mode[0] == LL_GPIO_MODE_ALTERNATE && fake_gpioa.af[0] == LL_GPIO_AF_13);
    CHECK(fake_gpioa.mode[1] == LL_GPIO_MODE_ALTERNATE && fake_gpioa.af[1] == LL_GPIO_AF_13);
#endif
    CHECK(fake_tim1.CCR3 == 0 && fake_tim1.CCR4 == 0 && (fake_tim1.BDTR & TIM_BDTR_MOE));
}

#if DIAG_REPORT
static void test_diag_led_follows_pa4(void) {
    module_start();
    fake_gpioa.ODR = 0;
    fake_gpioa.IDR = ZC_MASK;
    run_exti(500);
    CHECK((fake_gpioa.ODR & LED_PIN) == 0);                 /* PA4 high: LED (active low) on */
    fake_gpioa.IDR = 0;
    run_exti(1000);
    CHECK((fake_gpioa.ODR & LED_PIN) != 0);
}
#endif
#endif /* !SIM_ZC */

static void test_main_loop_changes_faders_with_irqs_off(void) {
    module_start();
    dim_profile_t p;
    dim_profile_default(&p);
    g_fader_changes = g_fader_changes_irq_on = 0;
    for (uint8_t ch = 0; ch < 2; ch++) {
        phase_set(ch, 1, 700);
        phase_set_range(ch, 100, 900);
        phase_set_profile(ch, &p);
        phase_hold_start(ch, -1, 100, 900, 0);
        phase_set(ch, 0, 700);                               /* off during a hold: two stores */
        phase_hold_start(ch, +1, 100, 900, 1);
        (void)phase_hold_stop(ch);
        phase_hold_cancel(ch);
    }
    CHECK(g_fader_changes == 16);
    CHECK(g_fader_changes_irq_on == 0);
    CHECK(!fake_irq_off);                                    /* and back on afterwards */
}

#if SIM_ZC
/* ---- bench build: virtual 50 Hz from TIM16, guarded against real mains ---- */

static long g_next_virtual;

static void run_tim16(void) {
    set_now(g_next_virtual);
    CHECK(fake_tim16.CCR1 == (uint32_t)(g_next_virtual & 0xFFFF));
    unsigned from = fake_egr_writes;
    fake_tim16.SR |= TIM_SR_CC1IF;
    g_in_isr = 1;
    TIM16_IRQHandler();
    g_in_isr = 0;
    after_isr(from);
    g_next_virtual += 500;
}

/* Run the bench build until t, with PA4 edges at the given times. */
static void sim_run(long until, const long *pa4_edges, int n, int exti_works) {
    int i = 0;
    while (1) {
        long te = i < n ? pa4_edges[i] : 1L << 40;
        long tv = (fake_tim16.DIER & TIM_DIER_CC1IE) ? g_next_virtual : 1L << 40;
        long t  = te < tv ? te : tv;
        if (t > until) {
            break;
        }
        if (te <= tv) {
            g_pa4          = (uint8_t)!g_pa4;
            fake_gpioa.IDR = g_pa4 ? ZC_MASK : 0;
            i++;
            if (exti_works && (fake_exti.IMR & ZC_MASK)) {
                run_exti(te);
            }
        } else {
            run_tim16();
        }
    }
    set_now(until);
}

static void sim_start(void) {
    module_start();
    g_next_virtual = 500;
    reset_stats();
    g_tol = 1L << 30;                                    /* virtual edges: no real ones to match */
    phase_set(0, 1, 1000);
    phase_set(1, 1, 1000);
}

static int gates_shut_down(void) {
    return fake_gpioa.mode[0] == LL_GPIO_MODE_OUTPUT && fake_gpioa.mode[1] == LL_GPIO_MODE_OUTPUT &&
           (fake_gpioa.ODR & 3u) == 0 && (fake_tim1.BDTR & TIM_BDTR_MOE) == 0;
}

static void test_sim_pa4_set_up_as_on_mains(void) {
    sim_start();
    CHECK(fake_gpioa.mode[4] == LL_GPIO_MODE_INPUT && fake_gpioa.pull[4] == LL_GPIO_PULL_UP);
    CHECK((fake_exti.RTSR & ZC_MASK) && (fake_exti.FTSR & ZC_MASK) && (fake_exti.IMR & ZC_MASK));
    CHECK(fake_exti4_port == (int)LL_EXTI_CONFIG_PORTA && fake_nvic_enabled[EXTI4_15_IRQn]);
    CHECK(fake_nvic_prio[EXTI4_15_IRQn] < fake_nvic_prio[TIM16_IRQn]);   /* the guard first */
    CHECK(fake_nvic_prio[TIM16_IRQn] == IRQ_PRIO_ZC);
}

static void test_sim_waits_200_ms_then_drives(void) {
    sim_start();
    sim_run(10000, NULL, 0, 1);                          /* 200 ms: PA4 static */
    CHECK(g_st.ug_on == 0 && g_phase_dbg.zc_edges == 0);
    sim_run(20000, NULL, 0, 1);
    CHECK(g_st.ug_on > 10);
    CHECK(g_st.preload_bad == 0 && g_st.arr_bad == 0 && g_st.on_unlocked == 0);
    CHECK(!gates_shut_down());
}

static void test_sim_on_mains_never_drives(void) {
    /* the module is on the mains: real edges every 10 ms from any phase */
    for (long phase = 0; phase < 500; phase += 37) {
        long e[200];
        for (int k = 0; k < 200; k++) {
            e[k] = 3 + phase + 500L * k;
        }
        sim_start();
        sim_run(100000, e, 200, 1);
        CHECK(g_st.ug_on == 0);
        CHECK(gates_shut_down());
        CHECK((fake_tim16.DIER & TIM_DIER_CC1IE) == 0);
    }
}

static void test_sim_edge_after_start_shuts_down_at_once(void) {
    /* mains comes later (bench supply first): the first edge stops the gates */
    sim_start();
    sim_run(30000, NULL, 0, 1);
    CHECK(g_st.ug_on > 10);
    long e[3] = { 30123, 30623, 31123 };
    sim_run(30123, e, 1, 1);
    CHECK(gates_shut_down());
    int on = g_st.ug_on;
    sim_run(60000, e + 1, 2, 1);
    CHECK(g_st.ug_on == on);
    CHECK(gates_shut_down());
}

static void test_sim_level_check_without_the_exti(void) {
    /* second layer: the TIM16 ISR sees PA4 change even if no EXTI came */
    sim_start();
    long e[1] = { 2600 };
    sim_run(20000, e, 1, 0);
    CHECK(g_st.ug_on == 0);
    CHECK(gates_shut_down());
}

static void test_sim_poll_check_without_the_isrs(void) {
    /* third layer: the main loop sees PA4 change */
    sim_start();
    poll(100);
    fake_gpioa.IDR = 0;
    poll(150);
    CHECK(gates_shut_down());
    sim_run(30000, NULL, 0, 1);
    CHECK(g_st.ug_on == 0);
}

static void test_sim_shutdown_survives_a_main_loop_moder_write(void) {
    /* The EXTI shuts the gates down while the main loop is between the read
     * and the write of GPIOA->MODER (uart_init): the write puts PA0/PA1 back
     * to the timer, MOE off, floating. The next poll makes them GPIO low again. */
    sim_start();
    sim_run(30000, NULL, 0, 1);
    CHECK(g_st.ug_on > 10 && !gates_shut_down());
    GPIO_TypeDef before = fake_gpioa;                    /* the main loop reads MODER ... */
    long         e[1]   = { 30123 };
    sim_run(30123, e, 1, 1);                             /* ... the EXTI shuts down ... */
    CHECK(gates_shut_down());
    int on = g_st.ug_on;
    fake_gpioa.mode[0] = before.mode[0];                 /* ... the stale write lands */
    fake_gpioa.mode[1] = before.mode[1];
    CHECK(fake_gpioa.mode[0] == LL_GPIO_MODE_ALTERNATE && fake_gpioa.mode[1] == LL_GPIO_MODE_ALTERNATE);
    poll(30123 + POLL_TICKS);
    CHECK(gates_shut_down());
    for (long t = 30123 + 2 * POLL_TICKS; t < 60000; t += POLL_TICKS) {
        poll(t);
        CHECK(gates_shut_down());
    }
    CHECK(g_st.ug_on == on);
}
#endif /* SIM_ZC */

int main(void) {
    int failed = 0;
#define RUN(fn) failed += run_isolated(fn, #fn)
#if !SIM_ZC
    RUN(test_zero_cross_input_and_priorities);
    RUN(test_reload_above_every_compare);
    RUN(test_clean_50_and_60_hz_lock_and_drive);
    RUN(test_asymmetric_detector_locks_and_follows);
    RUN(test_half_cycles_more_unequal_than_8_percent_never_lock);
    RUN(test_noise_on_the_lock_completing_edge);
    RUN(test_sparse_noise_turns_on_only_at_real_edges);
    RUN(test_dense_noise_away_from_the_edge_is_ignored_once_locked);
    RUN(test_fixed_phase_train_near_the_edge_does_not_capture_the_lock);
    RUN(test_late_isrs_and_lost_edges);
    RUN(test_everything_at_once);
    RUN(test_mains_stops_gates_go_dark);
#if DIAG_REPORT
    RUN(test_diag_led_follows_pa4);
#endif
#else
    RUN(test_sim_pa4_set_up_as_on_mains);
    RUN(test_sim_waits_200_ms_then_drives);
    RUN(test_sim_on_mains_never_drives);
    RUN(test_sim_edge_after_start_shuts_down_at_once);
    RUN(test_sim_level_check_without_the_exti);
    RUN(test_sim_poll_check_without_the_isrs);
    RUN(test_sim_shutdown_survives_a_main_loop_moder_write);
#endif
    RUN(test_main_loop_changes_faders_with_irqs_off);
    const char *variant = SIM_ZC ? "SIM" : MEASURE ? "MEASURE" : DIAG_REPORT ? "DIAG" : "normal";
    if (failed) {
        printf("phase (%s): %d check(s) failed\n", variant, failed);
        return EXIT_FAILURE;
    }
    printf("phase (%s): all tests passed\n", variant);
    return EXIT_SUCCESS;
}

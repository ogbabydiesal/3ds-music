#include "synth.h"
#include "samples.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
static LightLock graph_lock;
#define GRAPH_LOCK()   LightLock_Lock(&graph_lock)
#define GRAPH_UNLOCK() LightLock_Unlock(&graph_lock)
#else
#define GRAPH_LOCK()   ((void)0)
#define GRAPH_UNLOCK() ((void)0)
#endif

// ------------------------------------------------------------ node classes ---

static const char *const noise_types[] = { "WHITE", "PINK", "BROWN" };
static const char *const wave_types[]  = { "SINE", "TRI", "SAW", "SQR", "CUST" };
static const char *const dir_types[]   = { "FWD", "REV", "BOTH" };
static const char *const off_on[]      = { "OFF", "ON" };
static const char *const flt_types[]   = { "LP", "HP", "BP", "NOTCH" };
static const char *const depth_types[] = { "LOW", "HIGH" };
static const char *const xf_curves[]   = { "LIN", "EQ PWR" };

const NodeClass syn_classes[NODE_CLASS_COUNT] = {
    [NODE_NOISE] = {
        .name = "NOISE", .has_input = false, .input_name = "", .nparams = 2,
        .params = {
            { "TYPE",  PARAM_ENUM,   0, 2, 0,   "",  noise_types, 3 },
            { "LEVEL", PARAM_LINEAR, 0, 1, 0.5f, "%" },
        },
    },
    [NODE_ENV] = {
        .name = "ENV", .has_input = true, .input_name = "AUDIO", .nparams = 6,
        .params = {
            { "ATTACK",  PARAM_EXP,    0.5f, 2000, 1,    "ms" },
            { "DECAY",   PARAM_EXP,    1,    4000, 120,  "ms" },
            { "SUSTAIN", PARAM_LINEAR, 0,    1,    0,    "%"  },
            { "RELEASE", PARAM_EXP,    1,    4000, 80,   "ms" },
            { "LENGTH",  PARAM_EXP,    1,    2000, 20,   "ms" },
            { "LEVEL",   PARAM_LINEAR, 0,    1,    0.8f, "%"  },
        },
    },
    [NODE_OSC] = {
        // Input bends pitch: freq * 2^(input * MOD). Patch an ENV in for sweeps.
        .name = "OSC", .has_input = true, .input_name = "PITCH", .nparams = 5,
        .params = {
            { "WAVE",  PARAM_ENUM,   0,     4,    0,    "",    wave_types, 5 },
            { "FREQ",  PARAM_EXP,    20,    4000, 220,  "Hz" },
            { "PW",    PARAM_LINEAR, 0.05f, 0.95f, 0.5f, "%" },
            { "MOD",   PARAM_LINEAR, 0,     4,    0,    "oct" },
            { "LEVEL", PARAM_LINEAR, 0,     1,    0.5f, "%" },
        },
    },
    [NODE_REVERB] = {
        // John Chowning's reverb as in STK's JCRev (STK license: see THIRD_PARTY_NOTICES.md).
        // 3 series allpasses, 4 parallel damped combs, two output delays for stereo. T60 = decay time to -60 dB.
        .name = "REVERB", .has_input = true, .input_name = "AUDIO", .stereo = true, .nparams = 3,
        .params = {
            { "T60",   PARAM_EXP,    0.1f, 20, 1.0f, "s" },
            { "MIX",   PARAM_LINEAR, 0,    1,  0.3f, "%" },
            { "LEVEL", PARAM_LINEAR, 0,    1,  1.0f, "%" },
        },
    },
    [NODE_GRAINS] = {
        // Granular texture processor: an implementation of the Mutable Instruments Beads
        // algorithm, written from its manual (credited in Settings > CREDITS). The input is
        // recorded into an 8 s buffer and replayed by up to 16 grains.
        .name = "GRAINS", .has_input = true, .input_name = "AUDIO", .stereo = true, .nparams = 10,
        .params = {
            { "TIME",     PARAM_LINEAR, 0,   1,    0.2f, "%"  },   // 0 newest .. 1 oldest audio
            { "SIZE",     PARAM_EXP,    10,  4000, 120,  "ms" },
            { "SHAPE",    PARAM_LINEAR, 0,   1,    0.5f, "%"  },   // 0 clicky, .5 smooth, 1 slow attack
            { "PITCH",    PARAM_LINEAR, -24, 24,   0,    "st" },
            { "DENSITY",  PARAM_LINEAR, -1,  1,    0.5f, "%"  },   // <0 regular, 0 seeds only, >0 random
            { "RANDOM",   PARAM_LINEAR, 0,   1,    0.2f, "%"  },
            { "FEEDBACK", PARAM_LINEAR, 0,   1,    0,    "%"  },
            { "MIX",      PARAM_LINEAR, 0,   1,    0.6f, "%"  },
            { "DIR",      PARAM_ENUM,   0,   2,    0,    "",   dir_types, 3 },
            { "FREEZE",   PARAM_ENUM,   0,   1,    0,    "",   off_on, 2 },
        },
    },
    [NODE_MIC] = {
        // The 3DS microphone (32.73 kHz, resampled). Only on while a MIC node exists.
        .name = "MIC", .has_input = false, .input_name = "", .nparams = 1,
        .params = {
            { "GAIN", PARAM_LINEAR, 0, 8, 2, "x" },
        },
    },
    [NODE_FILTER] = {
        // State-variable filter (topology-preserving / "trapezoidal" form).
        .name = "FILTER", .has_input = true, .input_name = "AUDIO", .nparams = 4,
        .params = {
            { "TYPE",   PARAM_ENUM,   0,    3,     0,      "",   flt_types, 4 },
            { "CUTOFF", PARAM_EXP,    20,   15000, 1200,   "Hz" },
            { "Q",      PARAM_EXP,    0.5f, 20,    0.707f, ""   },
            { "LEVEL",  PARAM_LINEAR, 0,    1,     1,      "%"  },
        },
    },
    [NODE_DELAY] = {
        // Feedback delay (up to 2 s) with a filter inside the feedback loop: every
        // repeat passes through it again. TIME changes glide, like tape.
        .name = "DELAY", .has_input = true, .input_name = "AUDIO", .nparams = 7,
        .params = {
            { "TIME",     PARAM_EXP,    1,    2000,  300,    "ms" },
            { "FEEDBACK", PARAM_LINEAR, 0,    0.95f, 0.45f,  "%"  },
            { "FILTER",   PARAM_ENUM,   0,    3,     0,      "",   flt_types, 4 },
            { "CUTOFF",   PARAM_EXP,    20,   15000, 2500,   "Hz" },
            { "Q",        PARAM_EXP,    0.5f, 20,    0.707f, ""   },
            { "MIX",      PARAM_LINEAR, 0,    1,     0.4f,   "%"  },
            { "LEVEL",    PARAM_LINEAR, 0,    1,     1,      "%"  },
        },
    },
    [NODE_CHORUS] = {
        // One-knob analog (BBD) style chorus: a short delay swept by a triangle LFO,
        // darkened like a bucket-brigade chip and mixed 50/50 with the dry signal.
        .name = "CHORUS", .has_input = true, .input_name = "AUDIO", .nparams = 4,
        .params = {
            { "RATE",  PARAM_EXP,    0.05f, 8, 0.8f, "Hz" },
            { "DEPTH", PARAM_ENUM,   0,     1, 1,    "",  depth_types, 2 },
            { "MIX",   PARAM_LINEAR, 0,     1, 0.5f, "%" },
            { "LEVEL", PARAM_LINEAR, 0,     1, 1,    "%" },
        },
    },
    [NODE_SAMPLE] = {
        // Plays a .wav from sdmc:/3ds/nodeTrix/samples/ from START on every pulse.
        .name = "SAMPLE", .has_input = false, .input_name = "", .nparams = 6,
        .params = {
            { "FILE",  PARAM_FILE,   0,   SAMPLES_MAX - 1, 0, "" },
            { "PITCH", PARAM_LINEAR, -24, 24, 0,    "st" },
            { "START", PARAM_LINEAR, 0,   1,  0,    "%"  },
            { "END",   PARAM_LINEAR, 0,   1,  1,    "%"  },
            { "LOOP",  PARAM_ENUM,   0,   1,  0,    "",  off_on, 2 },
            { "LEVEL", PARAM_LINEAR, 0,   1,  0.8f, "%"  },
        },
    },
    [NODE_XFADE] = {
        // Crossfades between input A (MIX 0%) and input B (MIX 100%). LIN gains
        // (1-m, m) dip ~3 dB in the middle for unrelated sounds; EQ PWR (cos, sin)
        // keeps the loudness steady. Stereo inputs stay stereo.
        .name = "XFADE", .has_input = true, .input_name = "A", .has_input2 = true, .input2_name = "B",
        .stereo = true, .nparams = 3,
        .params = {
            { "MIX",   PARAM_LINEAR, 0, 1, 0.5f, "%" },
            { "CURVE", PARAM_ENUM,   0, 1, 1,    "", xf_curves, 2 },
            { "LEVEL", PARAM_LINEAR, 0, 1, 1,    "%" },
        },
    },
};


// ------------------------------------------------------------- JCRev state ---
// Delay lengths from STK JCRev (44.1 kHz), rescaled to our rate, odd and prime.
static const int jc_len44[9] = { 1116, 1356, 1422, 1617, 225, 341, 441, 211, 179 };
static int jc_len[9];
#define JC_MAXLEN 1200          // >= longest rescaled length (1617 * 32000/44100 -> 1181)

typedef struct {
    float ap[3][JC_MAXLEN / 2]; int ap_i[3];    // allpasses (lengths 4..6, all < 600)
    float cb[4][JC_MAXLEN];     int cb_i[4];    // combs (lengths 0..3)
    float lp[4];                                // one-pole lowpass in each comb loop
    float od[2][JC_MAXLEN / 4]; int od_i[2];    // output delays (lengths 7, 8)
    float t60;                                  // T60 the comb coefficients were made for
    float cc[4];
} JCRev;
static JCRev jc[SYN_MAX_NODES];

static bool is_prime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; d++) if (n % d == 0) return false;
    return true;
}

static void jc_lengths(void) {
    double scaler = SYN_SAMPLE_RATE / 44100.0;
    for (int i = 0; i < 9; i++) {
        int d = (int)floor(scaler * jc_len44[i]);
        if ((d & 1) == 0) d++;
        while (!is_prime(d)) d += 2;
        jc_len[i] = d;
    }
}

static void jc_clear(int i) { memset(&jc[i], 0, sizeof jc[i]); }

// ------------------------------------------------------------ GRAINS state ---
#define GR_BUF      (SYN_SAMPLE_RATE * 8)       // 8 s recording buffer per node
#define GR_GRAINS   16

typedef struct {
    bool  on;
    float pos, step;            // read position (samples into the buffer) and increment
    float t, dt;                // envelope phase 0..1 and its increment
    float shape;                // envelope shape when the grain started
    float gl, gr;               // pan gains
} Grain;

typedef struct {
    int16_t *buf;               // malloc'd while the node exists (NULL if out of memory)
    int      w;                 // write head
    int      filled;            // samples recorded so far (up to GR_BUF)
    Grain    g[GR_GRAINS];
    float    until_next;        // samples to the next automatic grain
    float    fb;                // last wet output (mono) for feedback
    float    norm;              // smoothed active-grain count
    uint32_t rng;
    bool     seed;              // a shape pulse asked for a grain
} GrainsState;
static GrainsState gr[SYN_MAX_NODES];

static void gr_alloc(int i) {
    if (!gr[i].buf) gr[i].buf = (int16_t *)calloc(GR_BUF, sizeof(int16_t));
    else memset(gr[i].buf, 0, GR_BUF * sizeof(int16_t));
    int16_t *b = gr[i].buf;
    memset(&gr[i], 0, sizeof gr[i]);
    gr[i].buf = b;
    gr[i].rng = 0x2545F491u * (uint32_t)(i + 7);
}
static void gr_free(int i) { free(gr[i].buf); memset(&gr[i], 0, sizeof gr[i]); }

// ------------------------------------------------------ filter / effects ---

typedef struct {                // state-variable filter (TPT form)
    float ic1, ic2;
    float cut, q;               // what the coefficients were made for
    float a1, a2, a3, k;
    int   type;
} Svf;

static void svf_set(Svf *s, int type, float fc, float q) {
    s->type = type;
    if (fc == s->cut && q == s->q) return;
    s->cut = fc; s->q = q;
    float f = fc < 10 ? 10 : fc > SYN_SAMPLE_RATE * 0.47f ? SYN_SAMPLE_RATE * 0.47f : fc;
    float g = tanf((float)M_PI * f / SYN_SAMPLE_RATE);
    s->k = 1.0f / (q < 0.1f ? 0.1f : q);
    s->a1 = 1.0f / (1.0f + g * (g + s->k));
    s->a2 = g * s->a1;
    s->a3 = g * s->a2;
}

static inline float svf_tick(Svf *s, float v0) {
    float v3 = v0 - s->ic2;
    float v1 = s->a1 * s->ic1 + s->a2 * v3;
    float v2 = s->ic2 + s->a2 * s->ic1 + s->a3 * v3;
    s->ic1 = 2 * v1 - s->ic1;
    s->ic2 = 2 * v2 - s->ic2;
    switch (s->type) {
    case FT_HP:    return v0 - s->k * v1 - v2;
    case FT_BP:    return s->k * v1;             // 0 dB at the centre
    case FT_NOTCH: return v0 - s->k * v1;
    default:       return v2;                    // LP
    }
}

static inline float soft_clip(float x) {         // ~tanh, smooth up to |x| = 3
    if (x > 3) x = 3;
    if (x < -3) x = -3;
    return x * (27 + x * x) / (27 + 9 * x * x);
}

#define DLY_BUF  (SYN_SAMPLE_RATE * 2 + 64)
#define CHO_BUF  1024                             // 32 ms

typedef struct { Svf f; } FilterState;
typedef struct { float *buf; int w; float cur; Svf f; } DelayState;
typedef struct { float buf[CHO_BUF]; int w; float ph; Svf bbd; } ChorusState;
typedef struct { float pos; bool playing; } SampleState;

static FilterState flt[SYN_MAX_NODES];
static DelayState  dly[SYN_MAX_NODES];
static ChorusState cho[SYN_MAX_NODES];
static SampleState smp[SYN_MAX_NODES];

static void dly_free(int i) { free(dly[i].buf); memset(&dly[i], 0, sizeof dly[i]); }
static void dly_alloc(int i) {
    float *b = dly[i].buf ? dly[i].buf : (float *)malloc(DLY_BUF * sizeof(float));
    memset(&dly[i], 0, sizeof dly[i]);
    dly[i].buf = b;
    if (b) memset(b, 0, DLY_BUF * sizeof(float));
}

static void gr_alloc(int i);
static void gr_free(int i);
static void jc_clear(int i);

// Fresh runtime state for node slot i (its buffers allocated for `type`).
static void node_state_init(int i, int type) {
    jc_clear(i);
    memset(&flt[i], 0, sizeof flt[i]);
    memset(&cho[i], 0, sizeof cho[i]);
    memset(&smp[i], 0, sizeof smp[i]);
    if (type == NODE_GRAINS) gr_alloc(i); else gr_free(i);
    if (type == NODE_DELAY) dly_alloc(i); else dly_free(i);
}
static void node_state_free(int i) { gr_free(i); dly_free(i); }

SynNode syn_nodes[SYN_MAX_NODES];
static int order[SYN_MAX_NODES], norder = 0;    // evaluation order (inputs first)
static volatile uint32_t pending_triggers = 0;

static int file_count(void) { int n = samples_count(); return n < 1 ? 1 : n; }

float syn_param_to_norm(const ParamDef *d, float v) {
    if (d->kind == PARAM_FILE) return file_count() > 1 ? v / (file_count() - 1) : 0;
    if (d->kind == PARAM_EXP) return logf(v / d->min) / logf(d->max / d->min);
    return (v - d->min) / (d->max - d->min);
}

float syn_norm_to_param(const ParamDef *d, float u) {
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    if (d->kind == PARAM_EXP) return d->min * powf(d->max / d->min, u);
    if (d->kind == PARAM_FILE) return floorf(u * (file_count() - 1) + 0.5f);
    if (d->kind == PARAM_ENUM) return floorf(d->min + u * (d->max - d->min) + 0.5f);
    return d->min + u * (d->max - d->min);
}

static bool valid_src(int j) { return j >= 0 && j < SYN_MAX_NODES && syn_nodes[j].used; }

// Is `target` reachable by walking upstream (through both inputs) from `from`?
static bool upstream_has(int from, int target) {
    int stack[2 * SYN_MAX_NODES + 2], sp = 0;
    bool seen[SYN_MAX_NODES] = { false };
    stack[sp++] = from;
    while (sp) {
        int j = stack[--sp];
        if (!valid_src(j) || seen[j]) continue;
        if (j == target) return true;
        seen[j] = true;
        stack[sp++] = syn_nodes[j].input;
        stack[sp++] = syn_nodes[j].input2;
    }
    return false;
}

// Recompute evaluation order (every node after its inputs) and which tracks
// gate which envelopes (everything upstream of an output). Caller holds the lock.
static void rebuild(void) {
    int depth[SYN_MAX_NODES] = { 0 };
    for (int i = 0; i < SYN_MAX_NODES; i++) syn_nodes[i].trig_mask = 0;
    for (int pass = 0; pass < SYN_MAX_NODES; pass++) {    // longest path from a source
        bool changed = false;
        for (int i = 0; i < SYN_MAX_NODES; i++) {
            if (!syn_nodes[i].used) continue;
            int ins[2] = { syn_nodes[i].input, syn_nodes[i].input2 };
            for (int p = 0; p < 2; p++)
                if (valid_src(ins[p]) && depth[ins[p]] + 1 > depth[i] && depth[ins[p]] + 1 < SYN_MAX_NODES) {
                    depth[i] = depth[ins[p]] + 1;
                    changed = true;
                }
        }
        if (!changed) break;
    }
    norder = 0;
    for (int d = 0; d < SYN_MAX_NODES; d++)
        for (int i = 0; i < SYN_MAX_NODES; i++)
            if (syn_nodes[i].used && depth[i] == d) order[norder++] = i;
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        SynNode *n = &syn_nodes[i];
        if (!n->used || !n->outputs) continue;
        for (int j = 0; j < SYN_MAX_NODES; j++)
            if (syn_nodes[j].used && upstream_has(i, j)) syn_nodes[j].trig_mask |= n->outputs;
    }
}

static int add_node_locked(NodeType type, float x, float y) {
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        if (syn_nodes[i].used) continue;
        SynNode *n = &syn_nodes[i];
        memset(n, 0, sizeof *n);
        n->used = true;
        n->type = type;
        n->x = x; n->y = y;
        n->input = n->input2 = -1;
        n->outputs = 0;
        memset(n->mod_src, -1, sizeof n->mod_src);
        n->rng = 0x9E3779B9u * (uint32_t)(i + 1);
        node_state_init(i, type);
        const NodeClass *c = &syn_classes[type];
        for (int k = 0; k < c->nparams; k++) n->p[k] = c->params[k].def;
        for (int k = 0; k < SYN_WT_SIZE; k++)        // default custom wave: a lopsided bump
            n->wt[k] = sinf(2.0f * (float)M_PI * k / SYN_WT_SIZE) * (k < SYN_WT_SIZE / 2 ? 1.0f : 0.45f);
        return i;
    }
    return -1;
}

int syn_add_node(NodeType type, float x, float y) {
    GRAPH_LOCK();
    int i = add_node_locked(type, x, y);
    rebuild();
    GRAPH_UNLOCK();
    return i;
}

int syn_add_copy(const SynNode *src, float x, float y) {
    GRAPH_LOCK();
    int i = add_node_locked(src->type, x, y);
    if (i >= 0) {
        memcpy(syn_nodes[i].p, src->p, sizeof src->p);
        memcpy(syn_nodes[i].wt, src->wt, sizeof src->wt);
        memcpy(syn_nodes[i].mod_src, src->mod_src, sizeof src->mod_src);
    }
    rebuild();
    GRAPH_UNLOCK();
    return i;
}

void syn_delete_node(int i) {
    GRAPH_LOCK();
    syn_nodes[i].used = false;
    node_state_free(i);
    for (int j = 0; j < SYN_MAX_NODES; j++)
    {
        if (syn_nodes[j].input == i) syn_nodes[j].input = -1;
        if (syn_nodes[j].input2 == i) syn_nodes[j].input2 = -1;
    }
    rebuild();
    GRAPH_UNLOCK();
}

bool syn_connect_port(int src, int dst, int port) {
    if (src == dst || src < 0 || dst < 0 || src >= SYN_MAX_NODES || dst >= SYN_MAX_NODES) return false;
    if (!syn_nodes[src].used || !syn_nodes[dst].used) return false;
    const NodeClass *c = &syn_classes[syn_nodes[dst].type];
    if (port == 0 ? !c->has_input : port == 1 ? !c->has_input2 : true) return false;
    if (upstream_has(src, dst)) return false;             // would create a loop
    GRAPH_LOCK();
    if (port == 0) syn_nodes[dst].input = src; else syn_nodes[dst].input2 = src;
    rebuild();
    GRAPH_UNLOCK();
    return true;
}

bool syn_connect(int src, int dst) { return syn_connect_port(src, dst, 0); }

void syn_disconnect_port(int dst, int port) {
    GRAPH_LOCK();
    if (port == 0) syn_nodes[dst].input = -1; else syn_nodes[dst].input2 = -1;
    rebuild();
    GRAPH_UNLOCK();
}

void syn_disconnect(int dst) { syn_disconnect_port(dst, 0); }

int syn_node_input(int node, int port) {
    if (node < 0 || node >= SYN_MAX_NODES || !syn_nodes[node].used) return -1;
    return port == 0 ? syn_nodes[node].input : syn_nodes[node].input2;
}

void syn_set_param(int node, int k, float v) {
    GRAPH_LOCK();
    syn_nodes[node].p[k] = v;
    GRAPH_UNLOCK();
}

void syn_set_wave_point(int node, int i, float v) {
    if (i < 0 || i >= SYN_WT_SIZE) return;
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    GRAPH_LOCK();
    syn_nodes[node].wt[i] = v;
    GRAPH_UNLOCK();
}

void syn_set_mod_src(int node, int k, int seq) {
    if (k < 0 || k >= SYN_MAX_PARAMS) return;
    syn_nodes[node].mod_src[k] = (int8_t)(seq >= 0 && seq < 8 ? seq : -1);
}

float syn_clamp_param(const ParamDef *d, float v) {
    float hi = d->kind == PARAM_FILE ? file_count() - 1 : d->max;
    if (v < d->min) v = d->min;
    if (v > hi) v = hi;
    if (d->kind == PARAM_ENUM || d->kind == PARAM_FILE) v = floorf(v + 0.5f);
    return v;
}

void syn_toggle_output(int node, int track) {
    GRAPH_LOCK();
    syn_nodes[node].outputs ^= (uint8_t)(1u << track);
    rebuild();
    GRAPH_UNLOCK();
}

void syn_clear_outputs(int node) {
    GRAPH_LOCK();
    syn_nodes[node].outputs = 0;
    rebuild();
    GRAPH_UNLOCK();
}

void syn_replace_all(const SynNode src[SYN_MAX_NODES]) {
    GRAPH_LOCK();
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        SynNode *n = &syn_nodes[i];
        memset(n, 0, sizeof *n);
        if (src[i].used && src[i].type >= 0 && src[i].type < NODE_CLASS_COUNT) node_state_init(i, src[i].type);
        else node_state_free(i);
        if (!src[i].used || src[i].type < 0 || src[i].type >= NODE_CLASS_COUNT) continue;
        n->used = true;
        n->type = src[i].type;
        n->x = src[i].x; n->y = src[i].y;
        memcpy(n->p, src[i].p, sizeof n->p);
        memcpy(n->wt, src[i].wt, sizeof n->wt);
        n->input = src[i].input;
        n->input2 = src[i].input2;
        n->outputs = src[i].outputs & 0x1F;
        for (int k = 0; k < SYN_MAX_PARAMS; k++)
            n->mod_src[k] = (src[i].mod_src[k] >= 0 && src[i].mod_src[k] < 8) ? src[i].mod_src[k] : -1;
        n->rng = 0x9E3779B9u * (uint32_t)(i + 1);
        jc_clear(i);
    }
    for (int i = 0; i < SYN_MAX_NODES; i++) {          // drop links to missing nodes / missing ports
        SynNode *n = &syn_nodes[i];
        if (!n->used) continue;
        if (!valid_src(n->input) || !syn_classes[n->type].has_input) n->input = -1;
        if (!valid_src(n->input2) || !syn_classes[n->type].has_input2) n->input2 = -1;
    }
    for (int i = 0; i < SYN_MAX_NODES; i++) {          // ... and links that close a loop
        SynNode *n = &syn_nodes[i];
        if (n->used && upstream_has(n->input2, i)) n->input2 = -1;
        if (n->used && upstream_has(n->input, i)) n->input = -1;
    }
    rebuild();
    GRAPH_UNLOCK();
}

void syn_init(void) {
#ifdef __3DS__
    LightLock_Init(&graph_lock);
#endif
    memset(syn_nodes, 0, sizeof syn_nodes);
    jc_lengths();
    // Starter patch: white noise -> short envelope, played by the cube track
    int n = add_node_locked(NODE_NOISE, 24, 40);
    int e = add_node_locked(NODE_ENV, 130, 40);
    syn_nodes[e].input = n;
    syn_nodes[e].outputs = 1u << 0;
    rebuild();
}

// ------------------------------------------------------- sequencer slew ---
// Each S1..S8 output glides from its last value to the new one. LIN is a
// straight line; LOG moves fast at first and eases in (log(1+Kt)/log(1+K)).
// Both arrive in exactly slew_ms. Applied every MOD_BLOCK samples (1 ms).

#define MOD_BLOCK 32
#define SLEW_LOG_K 30.0f

typedef struct {
    bool  have, active;
    float from, to, cur;
    float t, dt;               // progress 0..1, per sample
    int   curve;
} ModSlew;

static ModSlew mslew[SYN_MOD_SEQS];

float syn_slew_shape(int curve, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    if (curve == SLEW_LOG) return logf(1.0f + SLEW_LOG_K * t) / logf(1.0f + SLEW_LOG_K);
    return t;
}

static void mod_apply(int seq, float v) {               // caller holds the lock
    for (int n = 0; n < SYN_MAX_NODES; n++) {
        SynNode *node = &syn_nodes[n];
        if (!node->used) continue;
        const NodeClass *cls = &syn_classes[node->type];
        for (int k = 0; k < cls->nparams; k++)
            if (node->mod_src[k] == seq) node->p[k] = syn_clamp_param(&cls->params[k], v);
    }
}

void syn_mod_target(int seq, float value, float slew_ms, int curve) {
    if (seq < 0 || seq >= SYN_MOD_SEQS) return;
    GRAPH_LOCK();
    ModSlew *m = &mslew[seq];
    float samples = slew_ms * SYN_SAMPLE_RATE / 1000.0f;
    if (!m->have || samples < 1) {                      // first value, or no slew: jump
        m->cur = m->to = value;
        m->active = false;
        mod_apply(seq, value);
    } else {
        m->from = m->cur;
        m->to = value;
        m->t = 0;
        m->dt = 1.0f / samples;
        m->curve = curve;
        m->active = true;
    }
    m->have = true;
    GRAPH_UNLOCK();
}

void syn_mod_reset(void) {
    GRAPH_LOCK();
    for (int i = 0; i < SYN_MOD_SEQS; i++) mslew[i].have = mslew[i].active = false;
    GRAPH_UNLOCK();
}

static void mod_tick(int nsamples) {                    // audio thread, lock held
    for (int i = 0; i < SYN_MOD_SEQS; i++) {
        ModSlew *m = &mslew[i];
        if (!m->active) continue;
        m->t += m->dt * nsamples;
        if (m->t >= 1) { m->t = 1; m->active = false; }
        m->cur = m->from + (m->to - m->from) * syn_slew_shape(m->curve, m->t);
        mod_apply(i, m->cur);
    }
}

void syn_trigger(int track) {
    if (track < 0 || track >= SYN_NTRACKS) return;
    __atomic_fetch_or(&pending_triggers, 1u << track, __ATOMIC_SEQ_CST);
}

// ------------------------------------------------------------------- DSP ---

static inline float white(SynNode *n) {
    uint32_t x = n->rng;                 // xorshift32
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    n->rng = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static inline float process_noise(SynNode *n) {
    float w = white(n), v;
    switch ((int)n->p[NOISE_TYPE]) {
    case 1:                               // pink (Paul Kellett's "economy" filter, music-dsp 2002)
        n->pk[0] = 0.99765f * n->pk[0] + w * 0.0990460f;
        n->pk[1] = 0.96300f * n->pk[1] + w * 0.2965164f;
        n->pk[2] = 0.57000f * n->pk[2] + w * 1.0526913f;
        v = (n->pk[0] + n->pk[1] + n->pk[2] + w * 0.1848f) * 0.40f;
        break;
    case 2:                               // brown: leaky integrated white
        n->brown = (n->brown + 0.02f * w) / 1.02f;
        v = n->brown * 8.8f;
        break;
    default:
        v = w;
    }
    return v * n->p[NOISE_LEVEL];
}

// Exponential approach coefficient: ~99% of the way in `ms`.
static inline float coef(float ms) {
    return expf(-4.6f / (ms * 0.001f * SYN_SAMPLE_RATE + 1.0f));
}

static void env_gate(SynNode *n) {
    n->env_stage = 1;                     // retrigger from the current level
    n->gate_left = (int)(n->p[ENV_LEN] * 0.001f * SYN_SAMPLE_RATE);
}

static inline float process_env(SynNode *n, float in) {
    switch (n->env_stage) {
    case 1: {                             // attack: linear
        float inc = 1.0f / (n->p[ENV_A] * 0.001f * SYN_SAMPLE_RATE + 1.0f);
        n->env += inc;
        if (n->env >= 1.0f) { n->env = 1.0f; n->env_stage = 2; }
        break;
    }
    case 2: {                             // decay toward sustain
        float s = n->p[ENV_S];
        n->env = s + (n->env - s) * coef(n->p[ENV_D]);
        break;
    }
    case 3:                               // release
        n->env *= coef(n->p[ENV_R]);
        if (n->env < 0.0001f) { n->env = 0; n->env_stage = 0; }
        break;
    }
    if (n->env_stage == 1 || n->env_stage == 2) {
        if (--n->gate_left <= 0) n->env_stage = 3;
    }
    return in * n->env * n->p[ENV_LEVEL];
}

// PolyBLEP: smooths the jumps in saw/square to reduce aliasing.
static inline float polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline float process_osc(SynNode *n, float in, bool has_in) {
    float f = n->p[OSC_FREQ];
    if (has_in && n->p[OSC_MOD] > 0) f *= exp2f(in * n->p[OSC_MOD]);
    float dt = f / SYN_SAMPLE_RATE;
    if (dt > 0.45f) dt = 0.45f;
    float t = n->phase, v;
    switch ((int)n->p[OSC_WAVE]) {
    case WAVE_TRI: v = 4.0f * fabsf(t - 0.5f) - 1.0f; break;
    case WAVE_SAW: v = 2.0f * t - 1.0f - polyblep(t, dt); break;
    case WAVE_SQR: {
        float pw = n->p[OSC_PW];
        float t2 = t - pw + 1.0f;
        if (t2 >= 1.0f) t2 -= 1.0f;
        v = (t < pw ? 1.0f : -1.0f) + polyblep(t, dt) - polyblep(t2, dt);
        break;
    }
    case WAVE_CUSTOM: {
        float x = t * SYN_WT_SIZE;
        int i = (int)x;
        float fr = x - i;
        float a = n->wt[i & (SYN_WT_SIZE - 1)], b = n->wt[(i + 1) & (SYN_WT_SIZE - 1)];
        v = a + (b - a) * fr;
        break;
    }
    default: v = sinf(2.0f * (float)M_PI * t);
    }
    t += dt;
    if (t >= 1.0f) t -= 1.0f;
    n->phase = t;
    return v * n->p[OSC_LEVEL];
}

// STK JCRev::tick, sample for sample. Writes left to n->out, right to n->out_r.
static inline void process_reverb(SynNode *n, int idx, float input) {
    JCRev *r = &jc[idx];
    if (r->t60 != n->p[REV_T60]) {
        r->t60 = n->p[REV_T60];
        for (int i = 0; i < 4; i++)
            r->cc[i] = powf(10.0f, -3.0f * jc_len[i] / (r->t60 * SYN_SAMPLE_RATE));
    }
    const float g = 0.7f;                       // allpassCoefficient_
    float x = input;
    for (int a = 0; a < 3; a++) {               // three allpasses in series
        int len = jc_len[4 + a], *pi = &r->ap_i[a];
        float d = r->ap[a][*pi];                // lastOut()
        float w = g * d + x;
        r->ap[a][*pi] = w;
        if (++*pi >= len) *pi = 0;
        x = -g * w + d;
    }
    float filtout = 0;
    for (int k = 0; k < 4; k++) {               // four combs in parallel
        int len = jc_len[k], *pi = &r->cb_i[k];
        float fb = r->cc[k] * r->cb[k][*pi];
        r->lp[k] = 0.8f * fb + 0.2f * r->lp[k]; // OnePole, pole 0.2
        float t = x + r->lp[k];
        r->cb[k][*pi] = t;
        if (++*pi >= len) *pi = 0;
        filtout += t;
    }
    float wet[2];
    for (int c = 0; c < 2; c++) {               // output delays decorrelate L and R
        int len = jc_len[7 + c], *pi = &r->od_i[c];
        wet[c] = r->od[c][*pi];
        r->od[c][*pi] = filtout;
        if (++*pi >= len) *pi = 0;
    }
    float mix = n->p[REV_MIX], dry = (1.0f - mix) * input, lvl = 0.7f * n->p[REV_LEVEL];
    n->out   = (mix * wet[0] + dry) * lvl;
    n->out_r = (mix * wet[1] + dry) * lvl;
}

// ----------------------------------------------------------------- GRAINS ---

static inline float gr_rand(GrainsState *s) {           // 0..1
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

// Envelope by SHAPE: 0 = rectangle with tiny fades (clicky), .5 = triangle,
// 1 = slow attack and a short release.
static inline float gr_env(float t, float shape) {
    if (shape < 0.5f) {
        float f = shape;
        if (f < 0.004f) f = 0.004f;
        float a = t / f, b = (1.0f - t) / f;
        float e = a < b ? a : b;
        return e > 1.0f ? 1.0f : e;
    }
    float a = shape;                                    // attack fraction .5 .. .97
    if (a > 0.97f) a = 0.97f;
    return t < a ? t / a : (1.0f - t) / (1.0f - a);
}

static void gr_start_grain(SynNode *n, GrainsState *s) {
    Grain *g = NULL;
    for (int i = 0; i < GR_GRAINS; i++) if (!s->g[i].on) { g = &s->g[i]; break; }
    if (!g || s->filled < SYN_SAMPLE_RATE / 100) return;  // all busy, or nothing recorded yet
    float rnd = n->p[GR_RANDOM], r2 = rnd * rnd;

    float semis = n->p[GR_PITCH] + (gr_rand(s) * 2 - 1) * 12.0f * r2;
    float ratio = exp2f(semis / 12.0f);
    float len = n->p[GR_SIZE] * 0.001f * SYN_SAMPLE_RATE * exp2f((gr_rand(s) * 2 - 1) * rnd);
    if (len < 64) len = 64;
    int dir = (int)n->p[GR_DIR];
    bool rev = dir == 1 || (dir == 2 && gr_rand(s) < 0.5f);
    float travel = len * ratio;                          // samples the read head covers

    // TIME: how far back the grain starts. Keep the whole grain inside recorded
    // audio and, while recording, behind the write head.
    bool frozen = n->p[GR_FREEZE] >= 0.5f;
    float avail = (float)s->filled - 2;
    float time = n->p[GR_TIME] + (gr_rand(s) * 2 - 1) * 0.25f * rnd;
    time = time < 0 ? 0 : time > 1 ? 1 : time;
    float lo, hi;                                        // allowed start delays (samples back)
    if (rev) { lo = 1; hi = avail - travel; }
    else {
        lo = frozen ? travel : (travel > len ? travel - len : 0) + 32;
        hi = avail;
        if (lo > avail) lo = avail;
    }
    if (hi < lo) hi = lo;
    float delay = lo + time * (hi - lo);
    if (travel > avail) { travel = avail; len = travel / ratio; }

    float p = (float)s->w - delay;
    while (p < 0) p += GR_BUF;
    g->pos = p;
    g->step = rev ? -ratio : ratio;
    g->t = 0;
    g->dt = 1.0f / len;
    g->shape = n->p[GR_SHAPE];
    float spread = rnd > 0.5f ? 1.0f : rnd * 2;          // random pan, wider with RANDOM
    if (n->p[GR_DENSITY] > 0.02f && spread < 0.5f) spread = 0.5f;   // random rate pans too (as Beads does)
    float pan = 0.5f + (gr_rand(s) - 0.5f) * spread;
    g->gl = cosf(pan * 1.5707963f) * 1.41421356f * 0.7071f;
    g->gr = sinf(pan * 1.5707963f) * 1.41421356f * 0.7071f;
    g->on = true;
}

static inline void process_grains(SynNode *n, int idx, float input) {
    GrainsState *s = &gr[idx];
    if (!s->buf) { n->out = n->out_r = input; return; }

    // record (input + feedback, softly limited) unless frozen
    if (n->p[GR_FREEZE] < 0.5f) {
        float fbk = n->p[GR_FEEDBACK];
        if (fbk > 0.95f) fbk = 0.95f;
        float x = input + fbk * s->fb;
        x = x / (1.0f + fabsf(x) * 0.5f) * 1.5f;          // soft limiter: |x| < 3 -> < 1.8
        if (x > 1) x = 1;
        if (x < -1) x = -1;
        s->buf[s->w] = (int16_t)(x * 32767.0f);
        if (++s->w >= GR_BUF) s->w = 0;
        if (s->filled < GR_BUF) s->filled++;
    }

    // grain scheduler: DENSITY < 0 regular, > 0 random, ~0 only shape pulses (seeds)
    float d = n->p[GR_DENSITY], ad = fabsf(d);
    if (s->seed) { s->seed = false; gr_start_grain(n, s); }
    if (ad > 0.02f) {
        float rate = 0.5f * exp2f((ad - 0.02f) / 0.98f * 8.0f);   // 0.5 .. 128 grains/s
        if (--s->until_next <= 0) {
            gr_start_grain(n, s);
            float mean = SYN_SAMPLE_RATE / rate;
            s->until_next = d < 0 ? mean : -mean * logf(1.0f - gr_rand(s) * 0.999f);
        }
    }

    // play grains
    float l = 0, r = 0;
    int active = 0;
    for (int i = 0; i < GR_GRAINS; i++) {
        Grain *g = &s->g[i];
        if (!g->on) continue;
        active++;
        int i0 = (int)g->pos;
        float fr = g->pos - i0;
        int i1 = i0 + 1 >= GR_BUF ? 0 : i0 + 1;
        float v = (s->buf[i0] + (s->buf[i1] - s->buf[i0]) * fr) * (1.0f / 32767.0f);
        v *= gr_env(g->t, g->shape);
        l += v * g->gl;
        r += v * g->gr;
        g->pos += g->step;
        if (g->pos >= GR_BUF) g->pos -= GR_BUF;
        if (g->pos < 0) g->pos += GR_BUF;
        g->t += g->dt;
        if (g->t >= 1.0f) g->on = false;
    }
    s->norm += 0.0005f * ((float)active - s->norm);       // overlapping grains: keep the level steady
    float gain = 1.0f / sqrtf(s->norm > 1 ? s->norm : 1);
    l *= gain; r *= gain;
    s->fb = 0.5f * (l + r);

    float mix = n->p[GR_MIX], dry = (1.0f - mix) * input;
    n->out   = mix * l + dry;
    n->out_r = mix * r + dry;
}

// ----------------------------------------------------- FILTER/DELAY/CHORUS ---

static inline float process_filter(SynNode *n, int idx, float in) {
    FilterState *s = &flt[idx];
    svf_set(&s->f, (int)n->p[FLT_TYPE], n->p[FLT_CUTOFF], n->p[FLT_Q]);
    return svf_tick(&s->f, in) * n->p[FLT_LEVEL];
}

static inline float process_delay(SynNode *n, int idx, float in) {
    DelayState *s = &dly[idx];
    if (!s->buf) return in;
    float target = n->p[DLY_TIME] * 0.001f * SYN_SAMPLE_RATE;
    if (s->cur <= 0) s->cur = target;
    s->cur += (target - s->cur) * 0.0006f;              // glide to a new TIME (~50 ms)
    float d = s->cur < 1 ? 1 : s->cur > DLY_BUF - 4 ? DLY_BUF - 4 : s->cur;
    float rp = (float)s->w - d;
    if (rp < 0) rp += DLY_BUF;
    int i0 = (int)rp, i1 = i0 + 1 >= DLY_BUF ? 0 : i0 + 1;
    float fr = rp - i0;
    float y = s->buf[i0] + (s->buf[i1] - s->buf[i0]) * fr;
    svf_set(&s->f, (int)n->p[DLY_FTYPE], n->p[DLY_CUTOFF], n->p[DLY_Q]);
    float wet = svf_tick(&s->f, y);                     // every repeat goes through the filter
    s->buf[s->w] = soft_clip(in + n->p[DLY_FEEDBACK] * wet);
    if (++s->w >= DLY_BUF) s->w = 0;
    float mix = n->p[DLY_MIX];
    return ((1 - mix) * in + mix * wet) * n->p[DLY_LEVEL];
}

static inline float process_chorus(SynNode *n, int idx, float in) {
    ChorusState *s = &cho[idx];
    s->ph += n->p[CHO_RATE] / SYN_SAMPLE_RATE;
    if (s->ph >= 1) s->ph -= 1;
    float tri = 4 * fabsf(s->ph - 0.5f) - 1;            // triangle LFO, -1..1
    bool deep = n->p[CHO_DEPTH] >= 0.5f;
    float ms = deep ? 6.5f + 4.5f * tri : 4.0f + 1.6f * tri;   // the depth switch
    float d = ms * 0.001f * SYN_SAMPLE_RATE;
    s->buf[s->w] = soft_clip(in * 1.2f) / 1.2f;          // a little BBD drive
    float rp = (float)s->w - d;
    if (rp < 0) rp += CHO_BUF;
    int i0 = (int)rp, i1 = i0 + 1 >= CHO_BUF ? 0 : i0 + 1;
    float fr = rp - i0;
    float wet = s->buf[i0] + (s->buf[i1] - s->buf[i0]) * fr;
    if (++s->w >= CHO_BUF) s->w = 0;
    svf_set(&s->bbd, FT_LP, 5500, 0.6f);                // bucket-brigade darkening
    wet = svf_tick(&s->bbd, wet);
    float mix = n->p[CHO_MIX];
    return ((1 - mix) * in + mix * wet) * n->p[CHO_LEVEL];
}

// ------------------------------------------------------------------ SAMPLE ---

static inline float process_sample(SynNode *n, int idx) {
    SampleState *s = &smp[idx];
    int cnt = samples_count();
    if (!s->playing || cnt < 1) return 0;
    const Sample *sm = samples_get(((int)n->p[SMP_FILE] % cnt + cnt) % cnt);
    float a = n->p[SMP_START], b = n->p[SMP_END];
    if (b < a) { float t = a; a = b; b = t; }
    float start = a * sm->len, end = b * sm->len;
    if (end - start < 2) end = start + 2;
    if (end > sm->len - 1) end = (float)(sm->len - 1);
    if (s->pos < start) s->pos = start;
    if (s->pos >= end) {
        if (n->p[SMP_LOOP] >= 0.5f && end - start >= 2) s->pos = start;
        else { s->playing = false; return 0; }
    }
    int i0 = (int)s->pos;
    float fr = s->pos - i0;
    float v = (sm->data[i0] + (sm->data[i0 + 1] - sm->data[i0]) * fr) * (1.0f / 32768.0f);
    s->pos += (float)sm->rate / SYN_SAMPLE_RATE * exp2f(n->p[SMP_PITCH] / 12.0f);
    return v * n->p[SMP_LEVEL];
}

static void sample_trigger(SynNode *n, int idx) {
    smp[idx].playing = true;
    smp[idx].pos = 0;                                   // clamped up to START on the next sample
    (void)n;
}

// ------------------------------------------------------------------- XFADE ---

static struct { float m; int curve; float ga, gb; bool set; } xf[SYN_MAX_NODES];   // cached gains

static inline void src_lr(int j, float *l, float *r) {
    if (j < 0) { *l = *r = 0; return; }
    const SynNode *s = &syn_nodes[j];
    *l = s->out;
    *r = syn_classes[s->type].stereo ? s->out_r : s->out;
}

static inline void process_xfade(SynNode *n, int idx) {
    float m = n->p[XF_MIX];
    int curve = (int)n->p[XF_CURVE];
    if (!xf[idx].set || m != xf[idx].m || curve != xf[idx].curve) {
        xf[idx].m = m; xf[idx].curve = curve; xf[idx].set = true;
        if (curve == XC_EQPOWER) { xf[idx].ga = cosf(m * (0.5f * (float)M_PI)); xf[idx].gb = sinf(m * (0.5f * (float)M_PI)); }
        else { xf[idx].ga = 1.0f - m; xf[idx].gb = m; }
    }
    float al, ar, bl, br;
    src_lr(n->input, &al, &ar);
    src_lr(n->input2, &bl, &br);
    float lvl = n->p[XF_LEVEL];
    n->out   = (al * xf[idx].ga + bl * xf[idx].gb) * lvl;
    n->out_r = (ar * xf[idx].ga + br * xf[idx].gb) * lvl;
}

// --------------------------------------------------------------------- MIC ---

#ifdef __3DS__
#define MIC_SHMEM   0x30000
static u8 *mic_buf = NULL;
static u32 mic_bytes = 0;                               // sample data size inside mic_buf
static volatile bool mic_running = false;
static u32 mic_wr = 0;                                  // writer position (samples), read once per block
static float mic_rp = 0;
#endif
static float mic_x1 = 0, mic_y1 = 0;                    // DC blocker

static inline float process_mic(SynNode *n) {
    float x = 0;
#ifdef __3DS__
    if (mic_running && mic_bytes) {
        u32 ns = mic_bytes / 2;
        float dist = (float)mic_wr - mic_rp;
        if (dist < 0) dist += ns;
        if (dist < 32 || dist > 4000) {                 // (re)start ~25 ms behind the writer
            mic_rp = (float)mic_wr - 800;
            if (mic_rp < 0) mic_rp += ns;
        }
        const int16_t *pcm = (const int16_t *)mic_buf;
        int i0 = (int)mic_rp, i1 = (u32)(i0 + 1) >= ns ? 0 : i0 + 1;
        float fr = mic_rp - i0;
        x = (pcm[i0] + (pcm[i1] - pcm[i0]) * fr) * (1.0f / 32768.0f);
        mic_rp += 32730.0f / SYN_SAMPLE_RATE;           // the mic runs at 32.73 kHz
        if (mic_rp >= ns) mic_rp -= ns;
    }
#endif
    float y = x - mic_x1 + 0.995f * mic_y1;
    mic_x1 = x; mic_y1 = y;
    return y * n->p[MIC_GAIN];
}

static bool any_mic_node(void) {
    for (int i = 0; i < SYN_MAX_NODES; i++)
        if (syn_nodes[i].used && syn_nodes[i].type == NODE_MIC) return true;
    return false;
}

#ifdef __3DS__
void syn_mic_poll(void) {
    bool want = any_mic_node();
    if (want && !mic_running) {
        if (!mic_buf) mic_buf = (u8 *)memalign(0x1000, MIC_SHMEM);
        if (!mic_buf) return;
        if (R_FAILED(micInit(mic_buf, MIC_SHMEM))) return;
        mic_bytes = micGetSampleDataSize();
        if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_32730, 0, mic_bytes, true))) {
            micExit();
            return;
        }
        mic_wr = micGetLastSampleOffset() / 2;
        mic_rp = (float)mic_wr;
        GRAPH_LOCK();
        mic_running = true;
        GRAPH_UNLOCK();
    } else if (!want && mic_running) {
        syn_mic_stop();
    }
}

void syn_mic_stop(void) {
    if (!mic_running) return;
    GRAPH_LOCK();                                       // audio thread stops reading first
    mic_running = false;
    GRAPH_UNLOCK();
    MICU_StopSampling();
    micExit();
}

bool syn_mic_on(void) { return mic_running; }
#else
void syn_mic_poll(void) { (void)any_mic_node; }
void syn_mic_stop(void) {}
bool syn_mic_on(void) { return false; }
#endif

void syn_render(int16_t *out, int nframes) {
    GRAPH_LOCK();
    uint32_t trig = __atomic_exchange_n(&pending_triggers, 0, __ATOMIC_SEQ_CST);
    if (trig)
        for (int i = 0; i < SYN_MAX_NODES; i++) {
            SynNode *n = &syn_nodes[i];
            if (n->used && n->type == NODE_ENV && (n->trig_mask & trig)) env_gate(n);
            if (n->used && n->type == NODE_GRAINS && (n->trig_mask & trig)) gr[i].seed = true;
            if (n->used && n->type == NODE_SAMPLE && (n->trig_mask & trig)) sample_trigger(n, i);
        }

#ifdef __3DS__
    if (mic_running) mic_wr = micGetLastSampleOffset() / 2;
#endif
    for (int s = 0; s < nframes; s++) {
        if (s % MOD_BLOCK == 0) mod_tick(nframes - s < MOD_BLOCK ? nframes - s : MOD_BLOCK);
        float mix = 0, mix_r = 0;
        for (int o = 0; o < norder; o++) {
            SynNode *n = &syn_nodes[order[o]];
            bool has_in = n->input >= 0;
            float in = has_in ? syn_nodes[n->input].out : 0.0f;
            if (has_in && syn_classes[syn_nodes[n->input].type].stereo)  // stereo source: use the mid
                in = 0.5f * (in + syn_nodes[n->input].out_r);
            switch (n->type) {
            case NODE_NOISE: n->out = process_noise(n); break;
            case NODE_ENV:   n->out = process_env(n, has_in ? in : 1.0f); break;   // alone: its contour
            case NODE_OSC:   n->out = process_osc(n, in, has_in); break;
            case NODE_REVERB: process_reverb(n, order[o], in); break;
            case NODE_GRAINS:  process_grains(n, order[o], in); break;
            case NODE_MIC:     n->out = process_mic(n); break;
            case NODE_FILTER:  n->out = process_filter(n, order[o], in); break;
            case NODE_DELAY:   n->out = process_delay(n, order[o], in); break;
            case NODE_CHORUS:  n->out = process_chorus(n, order[o], in); break;
            case NODE_SAMPLE:  n->out = process_sample(n, order[o]); break;
            case NODE_XFADE:   process_xfade(n, order[o]); break;
            default:         n->out = 0;
            }
            if (n->outputs) {
                mix += n->out;
                mix_r += syn_classes[n->type].stereo ? n->out_r : n->out;
            }
        }
        float ch[2] = { mix * 0.9f, mix_r * 0.9f };
        for (int c = 0; c < 2; c++) {
            if (ch[c] > 1.0f) ch[c] = 1.0f;
            if (ch[c] < -1.0f) ch[c] = -1.0f;
            out[2 * s + c] = (int16_t)(ch[c] * 32000.0f);
        }
    }
    GRAPH_UNLOCK();
}

// ----------------------------------------------------------- 3DS output ---

#ifdef __3DS__

#define BUF_SAMPLES 256
#define NBUFS       3

static ndspWaveBuf wbuf[NBUFS];
static int16_t *audio_mem = NULL;
static LightEvent audio_event;
static volatile bool audio_run = false;
static Thread audio_thread = NULL;
static bool audio_ok = false;

static void ndsp_callback(void *data) { (void)data; LightEvent_Signal(&audio_event); }

static void audio_fn(void *arg) {
    (void)arg;
    while (audio_run) {
        for (int i = 0; i < NBUFS; i++) {
            if (wbuf[i].status != NDSP_WBUF_DONE && wbuf[i].status != NDSP_WBUF_FREE) continue;
            syn_render(wbuf[i].data_pcm16, BUF_SAMPLES);
            DSP_FlushDataCache(wbuf[i].data_pcm16, BUF_SAMPLES * 2 * sizeof(int16_t));
            ndspChnWaveBufAdd(0, &wbuf[i]);
        }
        LightEvent_Wait(&audio_event);
    }
}

bool syn_audio_start(void) {
    if (R_FAILED(ndspInit())) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, SYN_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 0 };
    mix[0] = mix[1] = 1.0f;                  // L -> front left, R -> front right
    ndspChnSetMix(0, mix);

    audio_mem = (int16_t *)linearAlloc(NBUFS * BUF_SAMPLES * 2 * sizeof(int16_t));
    if (!audio_mem) { ndspExit(); return false; }
    memset(audio_mem, 0, NBUFS * BUF_SAMPLES * 2 * sizeof(int16_t));
    memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NBUFS; i++) {
        wbuf[i].data_vaddr = audio_mem + i * BUF_SAMPLES * 2;
        wbuf[i].nsamples = BUF_SAMPLES;
    }

    LightEvent_Init(&audio_event, RESET_ONESHOT);
    ndspSetCallback(ndsp_callback, NULL);
    audio_run = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    audio_thread = threadCreate(audio_fn, NULL, 32 * 1024, prio - 2, -1, false);
    audio_ok = audio_thread != NULL;
    return audio_ok;
}

void syn_audio_stop(void) {
    if (!audio_mem) return;
    audio_run = false;
    if (audio_thread) {
        LightEvent_Signal(&audio_event);
        threadJoin(audio_thread, U64_MAX);
        threadFree(audio_thread);
        audio_thread = NULL;
    }
    ndspChnWaveBufClear(0);
    ndspExit();
    linearFree(audio_mem);
    audio_mem = NULL;
    audio_ok = false;
}

bool syn_audio_ok(void) { return audio_ok; }

#else
bool syn_audio_start(void) { return false; }
void syn_audio_stop(void) {}
#ifndef SYN_HOST_AUDIO_OK
#define SYN_HOST_AUDIO_OK 0
#endif
bool syn_audio_ok(void) { return SYN_HOST_AUDIO_OK; }   // host builds (tests, UI renderer)
#endif

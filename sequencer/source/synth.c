#include "synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>
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
        // John Chowning's reverb as in STK's JCRev: 3 series allpasses, 4 parallel
        // damped combs, two output delays for stereo. T60 = decay time to -60 dB.
        .name = "REVERB", .has_input = true, .input_name = "AUDIO", .stereo = true, .nparams = 3,
        .params = {
            { "T60",   PARAM_EXP,    0.1f, 20, 1.0f, "s" },
            { "MIX",   PARAM_LINEAR, 0,    1,  0.3f, "%" },
            { "LEVEL", PARAM_LINEAR, 0,    1,  1.0f, "%" },
        },
    },
    [NODE_BEADS] = {
        // Granular texture processor after Mutable Instruments Beads: the input is
        // recorded into an 8 s buffer and replayed by up to 16 grains.
        .name = "BEADS", .has_input = true, .input_name = "AUDIO", .stereo = true, .nparams = 10,
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

// ------------------------------------------------------------- BEADS state ---
#define BD_BUF      (SYN_SAMPLE_RATE * 8)       // 8 s recording buffer per node
#define BD_GRAINS   16

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
    int      filled;            // samples recorded so far (up to BD_BUF)
    Grain    g[BD_GRAINS];
    float    until_next;        // samples to the next automatic grain
    float    fb;                // last wet output (mono) for feedback
    float    norm;              // smoothed active-grain count
    uint32_t rng;
    bool     seed;              // a shape pulse asked for a grain
} BeadsState;
static BeadsState bd[SYN_MAX_NODES];

static void bd_alloc(int i) {
    if (!bd[i].buf) bd[i].buf = (int16_t *)calloc(BD_BUF, sizeof(int16_t));
    else memset(bd[i].buf, 0, BD_BUF * sizeof(int16_t));
    int16_t *b = bd[i].buf;
    memset(&bd[i], 0, sizeof bd[i]);
    bd[i].buf = b;
    bd[i].rng = 0x2545F491u * (uint32_t)(i + 7);
}
static void bd_free(int i) { free(bd[i].buf); memset(&bd[i], 0, sizeof bd[i]); }

SynNode syn_nodes[SYN_MAX_NODES];
static int order[SYN_MAX_NODES], norder = 0;    // evaluation order (inputs first)
static volatile uint32_t pending_triggers = 0;

float syn_param_to_norm(const ParamDef *d, float v) {
    if (d->kind == PARAM_EXP) return logf(v / d->min) / logf(d->max / d->min);
    return (v - d->min) / (d->max - d->min);
}

float syn_norm_to_param(const ParamDef *d, float u) {
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    if (d->kind == PARAM_EXP) return d->min * powf(d->max / d->min, u);
    if (d->kind == PARAM_ENUM) return floorf(d->min + u * (d->max - d->min) + 0.5f);
    return d->min + u * (d->max - d->min);
}

// Recompute evaluation order and which tracks gate which envelopes.
// Caller holds the graph lock.
static void rebuild(void) {
    int depth[SYN_MAX_NODES];
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        depth[i] = 0;
        syn_nodes[i].trig_mask = 0;
        if (!syn_nodes[i].used) continue;
        for (int j = syn_nodes[i].input; j >= 0 && depth[i] < SYN_MAX_NODES; j = syn_nodes[j].input) depth[i]++;
    }
    norder = 0;
    for (int d = 0; d < SYN_MAX_NODES; d++)
        for (int i = 0; i < SYN_MAX_NODES; i++)
            if (syn_nodes[i].used && depth[i] == d) order[norder++] = i;
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        SynNode *n = &syn_nodes[i];
        if (!n->used || !n->outputs) continue;
        for (int j = i; j >= 0; j = syn_nodes[j].input) syn_nodes[j].trig_mask |= n->outputs;
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
        n->input = -1;
        n->outputs = 0;
        memset(n->mod_src, -1, sizeof n->mod_src);
        n->rng = 0x9E3779B9u * (uint32_t)(i + 1);
        jc_clear(i);
        if (type == NODE_BEADS) bd_alloc(i); else bd_free(i);
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
    bd_free(i);
    for (int j = 0; j < SYN_MAX_NODES; j++)
        if (syn_nodes[j].input == i) syn_nodes[j].input = -1;
    rebuild();
    GRAPH_UNLOCK();
}

bool syn_connect(int src, int dst) {
    if (src == dst || src < 0 || dst < 0) return false;
    if (!syn_nodes[src].used || !syn_nodes[dst].used) return false;
    if (!syn_classes[syn_nodes[dst].type].has_input) return false;
    for (int j = src; j >= 0; j = syn_nodes[j].input)     // would create a loop?
        if (j == dst) return false;
    GRAPH_LOCK();
    syn_nodes[dst].input = src;
    rebuild();
    GRAPH_UNLOCK();
    return true;
}

void syn_disconnect(int dst) {
    GRAPH_LOCK();
    syn_nodes[dst].input = -1;
    rebuild();
    GRAPH_UNLOCK();
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
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    if (d->kind == PARAM_ENUM) v = floorf(v + 0.5f);
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
        bool want_beads = src[i].used && src[i].type == NODE_BEADS;
        if (want_beads) bd_alloc(i); else bd_free(i);
        if (!src[i].used || src[i].type < 0 || src[i].type >= NODE_CLASS_COUNT) continue;
        n->used = true;
        n->type = src[i].type;
        n->x = src[i].x; n->y = src[i].y;
        memcpy(n->p, src[i].p, sizeof n->p);
        memcpy(n->wt, src[i].wt, sizeof n->wt);
        n->input = src[i].input;
        n->outputs = src[i].outputs & 0x1F;
        for (int k = 0; k < SYN_MAX_PARAMS; k++)
            n->mod_src[k] = (src[i].mod_src[k] >= 0 && src[i].mod_src[k] < 8) ? src[i].mod_src[k] : -1;
        n->rng = 0x9E3779B9u * (uint32_t)(i + 1);
        jc_clear(i);
    }
    for (int i = 0; i < SYN_MAX_NODES; i++) {          // drop links to missing nodes / loops
        SynNode *n = &syn_nodes[i];
        if (!n->used || n->input < 0) continue;
        if (n->input >= SYN_MAX_NODES || !syn_nodes[n->input].used || !syn_classes[n->type].has_input) n->input = -1;
    }
    for (int i = 0; i < SYN_MAX_NODES; i++) {
        int steps = 0;
        for (int j = syn_nodes[i].input; j >= 0 && steps <= SYN_MAX_NODES; j = syn_nodes[j].input) steps++;
        if (steps > SYN_MAX_NODES) syn_nodes[i].input = -1;
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
    case 1:                               // pink (Paul Kellet, economy version)
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

// ------------------------------------------------------------------ BEADS ---

static inline float bd_rand(BeadsState *s) {           // 0..1
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

// Envelope by SHAPE: 0 = rectangle with tiny fades (clicky), .5 = triangle,
// 1 = slow attack and a short release.
static inline float bd_env(float t, float shape) {
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

static void bd_start_grain(SynNode *n, BeadsState *s) {
    Grain *g = NULL;
    for (int i = 0; i < BD_GRAINS; i++) if (!s->g[i].on) { g = &s->g[i]; break; }
    if (!g || s->filled < SYN_SAMPLE_RATE / 100) return;  // all busy, or nothing recorded yet
    float rnd = n->p[BD_RANDOM], r2 = rnd * rnd;

    float semis = n->p[BD_PITCH] + (bd_rand(s) * 2 - 1) * 12.0f * r2;
    float ratio = exp2f(semis / 12.0f);
    float len = n->p[BD_SIZE] * 0.001f * SYN_SAMPLE_RATE * exp2f((bd_rand(s) * 2 - 1) * rnd);
    if (len < 64) len = 64;
    int dir = (int)n->p[BD_DIR];
    bool rev = dir == 1 || (dir == 2 && bd_rand(s) < 0.5f);
    float travel = len * ratio;                          // samples the read head covers

    // TIME: how far back the grain starts. Keep the whole grain inside recorded
    // audio and, while recording, behind the write head.
    bool frozen = n->p[BD_FREEZE] >= 0.5f;
    float avail = (float)s->filled - 2;
    float time = n->p[BD_TIME] + (bd_rand(s) * 2 - 1) * 0.25f * rnd;
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
    while (p < 0) p += BD_BUF;
    g->pos = p;
    g->step = rev ? -ratio : ratio;
    g->t = 0;
    g->dt = 1.0f / len;
    g->shape = n->p[BD_SHAPE];
    float spread = rnd > 0.5f ? 1.0f : rnd * 2;          // random pan, wider with RANDOM
    if (n->p[BD_DENSITY] > 0.02f && spread < 0.5f) spread = 0.5f;   // random rate: Beads pans too
    float pan = 0.5f + (bd_rand(s) - 0.5f) * spread;
    g->gl = cosf(pan * 1.5707963f) * 1.41421356f * 0.7071f;
    g->gr = sinf(pan * 1.5707963f) * 1.41421356f * 0.7071f;
    g->on = true;
}

static inline void process_beads(SynNode *n, int idx, float input) {
    BeadsState *s = &bd[idx];
    if (!s->buf) { n->out = n->out_r = input; return; }

    // record (input + feedback, softly limited) unless frozen
    if (n->p[BD_FREEZE] < 0.5f) {
        float fbk = n->p[BD_FEEDBACK];
        if (fbk > 0.95f) fbk = 0.95f;
        float x = input + fbk * s->fb;
        x = x / (1.0f + fabsf(x) * 0.5f) * 1.5f;          // soft limiter: |x| < 3 -> < 1.8
        if (x > 1) x = 1;
        if (x < -1) x = -1;
        s->buf[s->w] = (int16_t)(x * 32767.0f);
        if (++s->w >= BD_BUF) s->w = 0;
        if (s->filled < BD_BUF) s->filled++;
    }

    // grain scheduler: DENSITY < 0 regular, > 0 random, ~0 only shape pulses (seeds)
    float d = n->p[BD_DENSITY], ad = fabsf(d);
    if (s->seed) { s->seed = false; bd_start_grain(n, s); }
    if (ad > 0.02f) {
        float rate = 0.5f * exp2f((ad - 0.02f) / 0.98f * 8.0f);   // 0.5 .. 128 grains/s
        if (--s->until_next <= 0) {
            bd_start_grain(n, s);
            float mean = SYN_SAMPLE_RATE / rate;
            s->until_next = d < 0 ? mean : -mean * logf(1.0f - bd_rand(s) * 0.999f);
        }
    }

    // play grains
    float l = 0, r = 0;
    int active = 0;
    for (int i = 0; i < BD_GRAINS; i++) {
        Grain *g = &s->g[i];
        if (!g->on) continue;
        active++;
        int i0 = (int)g->pos;
        float fr = g->pos - i0;
        int i1 = i0 + 1 >= BD_BUF ? 0 : i0 + 1;
        float v = (s->buf[i0] + (s->buf[i1] - s->buf[i0]) * fr) * (1.0f / 32767.0f);
        v *= bd_env(g->t, g->shape);
        l += v * g->gl;
        r += v * g->gr;
        g->pos += g->step;
        if (g->pos >= BD_BUF) g->pos -= BD_BUF;
        if (g->pos < 0) g->pos += BD_BUF;
        g->t += g->dt;
        if (g->t >= 1.0f) g->on = false;
    }
    s->norm += 0.0005f * ((float)active - s->norm);       // overlapping grains: keep the level steady
    float gain = 1.0f / sqrtf(s->norm > 1 ? s->norm : 1);
    l *= gain; r *= gain;
    s->fb = 0.5f * (l + r);

    float mix = n->p[BD_MIX], dry = (1.0f - mix) * input;
    n->out   = mix * l + dry;
    n->out_r = mix * r + dry;
}

void syn_render(int16_t *out, int nframes) {
    GRAPH_LOCK();
    uint32_t trig = __atomic_exchange_n(&pending_triggers, 0, __ATOMIC_SEQ_CST);
    if (trig)
        for (int i = 0; i < SYN_MAX_NODES; i++) {
            SynNode *n = &syn_nodes[i];
            if (n->used && n->type == NODE_ENV && (n->trig_mask & trig)) env_gate(n);
            if (n->used && n->type == NODE_BEADS && (n->trig_mask & trig)) bd[i].seed = true;
        }

    for (int s = 0; s < nframes; s++) {
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
            case NODE_BEADS:  process_beads(n, order[o], in); break;
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

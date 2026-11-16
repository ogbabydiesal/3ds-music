#include "synth.h"

#include <math.h>
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
};

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
        n->rng = 0x9E3779B9u * (uint32_t)(i + 1);
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
    }
    rebuild();
    GRAPH_UNLOCK();
    return i;
}

void syn_delete_node(int i) {
    GRAPH_LOCK();
    syn_nodes[i].used = false;
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

void syn_init(void) {
#ifdef __3DS__
    LightLock_Init(&graph_lock);
#endif
    memset(syn_nodes, 0, sizeof syn_nodes);
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

void syn_render(int16_t *out, int nsamples) {
    GRAPH_LOCK();
    uint32_t trig = __atomic_exchange_n(&pending_triggers, 0, __ATOMIC_SEQ_CST);
    if (trig)
        for (int i = 0; i < SYN_MAX_NODES; i++) {
            SynNode *n = &syn_nodes[i];
            if (n->used && n->type == NODE_ENV && (n->trig_mask & trig)) env_gate(n);
        }

    for (int s = 0; s < nsamples; s++) {
        float mix = 0;
        for (int o = 0; o < norder; o++) {
            SynNode *n = &syn_nodes[order[o]];
            bool has_in = n->input >= 0;
            float in = has_in ? syn_nodes[n->input].out : 0.0f;
            switch (n->type) {
            case NODE_NOISE: n->out = process_noise(n); break;
            case NODE_ENV:   n->out = process_env(n, has_in ? in : 1.0f); break;   // alone: its contour
            case NODE_OSC:   n->out = process_osc(n, in, has_in); break;
            default:         n->out = 0;
            }
            if (n->outputs) mix += n->out;
        }
        mix *= 0.9f;
        if (mix > 1.0f) mix = 1.0f;
        if (mix < -1.0f) mix = -1.0f;
        out[s] = (int16_t)(mix * 32000.0f);
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
            DSP_FlushDataCache(wbuf[i].data_pcm16, BUF_SAMPLES * sizeof(int16_t));
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
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0 };
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(0, mix);

    audio_mem = (int16_t *)linearAlloc(NBUFS * BUF_SAMPLES * sizeof(int16_t));
    if (!audio_mem) { ndspExit(); return false; }
    memset(audio_mem, 0, NBUFS * BUF_SAMPLES * sizeof(int16_t));
    memset(wbuf, 0, sizeof wbuf);
    for (int i = 0; i < NBUFS; i++) {
        wbuf[i].data_vaddr = audio_mem + i * BUF_SAMPLES;
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

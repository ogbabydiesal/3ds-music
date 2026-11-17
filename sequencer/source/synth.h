// Tiny modular synth: a patch graph of nodes (noise, oscillator, envelope)
// rendered to 16-bit stereo audio. Each node has at most one input.
//
// A node is heard only if it is an OUTPUT for one or more sequencer shapes
// (a bitmask, empty by default). When a shape pulses, every envelope upstream
// of an output for that shape is triggered.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SYN_SAMPLE_RATE  32000
#define SYN_MAX_NODES    32
#define SYN_MAX_PARAMS   8
#define SYN_NTRACKS      5
#define SYN_WT_SIZE      16          // custom wave points

typedef enum { NODE_NOISE, NODE_ENV, NODE_OSC, NODE_REVERB, NODE_CLASS_COUNT } NodeType;

typedef enum { PARAM_LINEAR, PARAM_EXP, PARAM_ENUM } ParamKind;

typedef struct {
    const char *name;
    ParamKind   kind;
    float       min, max, def;
    const char *unit;               // "ms", "%", "Hz", "oct"
    const char *const *labels;      // PARAM_ENUM
    int         nlabels;
} ParamDef;

typedef struct {
    const char *name;               // shown in the carousel and on nodes
    bool        has_input;
    const char *input_name;         // what the input does, e.g. "AUDIO", "PITCH"
    int         nparams;
    ParamDef    params[SYN_MAX_PARAMS];
} NodeClass;

extern const NodeClass syn_classes[NODE_CLASS_COUNT];

// Parameter indices per class
enum { NOISE_TYPE, NOISE_LEVEL };
enum { ENV_A, ENV_D, ENV_S, ENV_R, ENV_LEN, ENV_LEVEL };
enum { OSC_WAVE, OSC_FREQ, OSC_PW, OSC_MOD, OSC_LEVEL };
enum { REV_T60, REV_MIX, REV_LEVEL };
enum { WAVE_SINE, WAVE_TRI, WAVE_SAW, WAVE_SQR, WAVE_CUSTOM };

typedef struct {
    bool     used;
    NodeType type;
    float    x, y;                  // canvas (world) position, UI only
    float    p[SYN_MAX_PARAMS];
    float    wt[SYN_WT_SIZE];       // custom waveform, -1..1
    int      input;                 // source node index, -1 = none
    uint8_t  outputs;               // bit t = output for shape t
    int8_t   mod_src[SYN_MAX_PARAMS];   // step sequencer (0..7) driving each param, -1 = none

    // runtime (audio thread)
    float    out;                   // mono (left for REVERB)
    float    out_r;                 // right channel (REVERB only)
    uint32_t rng;
    float    pk[3], brown;
    float    phase;
    int      env_stage;             // 0 idle, 1 attack, 2 decay/sustain, 3 release
    float    env;
    int      gate_left;
    uint8_t  trig_mask;
} SynNode;

extern SynNode syn_nodes[SYN_MAX_NODES];

void syn_init(void);                                // default patch
int  syn_add_node(NodeType type, float x, float y); // returns index or -1
int  syn_add_copy(const SynNode *src, float x, float y);   // params + wave, no links/outputs
void syn_delete_node(int i);
bool syn_connect(int src, int dst);                 // false if invalid / would loop
void syn_disconnect(int dst);
void syn_set_param(int node, int k, float v);
void syn_set_wave_point(int node, int i, float v);
void syn_toggle_output(int node, int track);
void syn_clear_outputs(int node);
void syn_set_mod_src(int node, int k, int seq);    // -1 = none
float syn_clamp_param(const ParamDef *d, float v);  // keep v inside the parameter's range
// Replace the whole patch (e.g. loading a slot). Copies used/type/x/y/p/wt/input/outputs.
void syn_replace_all(const SynNode src[SYN_MAX_NODES]);

float syn_param_to_norm(const ParamDef *d, float v);
float syn_norm_to_param(const ParamDef *d, float u);

void syn_trigger(int track);                        // safe from any thread
void syn_render(int16_t *out, int nframes);         // audio thread, interleaved stereo L,R

bool syn_audio_start(void);   // false if the DSP couldn't start (dspfirm.cdc missing)
void syn_audio_stop(void);
bool syn_audio_ok(void);

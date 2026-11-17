// Tiny modular synth: a patch graph of nodes (noise, oscillator, envelope)
// rendered to 16-bit stereo audio. Most nodes have one input; XFADE has two (A, B).
// Any node's output can feed any number of inputs.
//
// A node is heard only if it is an OUTPUT for one or more sequencer shapes
// (a bitmask, empty by default). When a shape pulses, every envelope upstream
// of an output for that shape is triggered.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SYN_SAMPLE_RATE  32000
#define SYN_MAX_NODES    32
#define SYN_MAX_PARAMS   10
#define SYN_NTRACKS      5
#define SYN_WT_SIZE      16          // custom wave points

typedef enum { NODE_NOISE, NODE_ENV, NODE_OSC, NODE_REVERB, NODE_GRAINS,
               NODE_MIC, NODE_FILTER, NODE_DELAY, NODE_CHORUS, NODE_SAMPLE, NODE_XFADE,
               NODE_CLASS_COUNT } NodeType;   // saved by number: append only

typedef enum { PARAM_LINEAR, PARAM_EXP, PARAM_ENUM, PARAM_FILE } ParamKind;   // FILE: index into samples.h

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
    bool        has_input2;         // second input (XFADE's B)
    const char *input2_name;
    bool        stereo;             // writes out (left) and out_r (right)
    int         nparams;
    ParamDef    params[SYN_MAX_PARAMS];
} NodeClass;

extern const NodeClass syn_classes[NODE_CLASS_COUNT];

// Parameter indices per class
enum { NOISE_TYPE, NOISE_LEVEL };
enum { ENV_A, ENV_D, ENV_S, ENV_R, ENV_LEN, ENV_LEVEL };
enum { OSC_WAVE, OSC_FREQ, OSC_PW, OSC_MOD, OSC_LEVEL };
enum { REV_T60, REV_MIX, REV_LEVEL };
enum { MIC_GAIN };
enum { FLT_TYPE, FLT_CUTOFF, FLT_Q, FLT_LEVEL };
enum { DLY_TIME, DLY_FEEDBACK, DLY_FTYPE, DLY_CUTOFF, DLY_Q, DLY_MIX, DLY_LEVEL };
enum { CHO_RATE, CHO_DEPTH, CHO_MIX, CHO_LEVEL };
enum { SMP_FILE, SMP_PITCH, SMP_START, SMP_END, SMP_LOOP, SMP_LEVEL };
enum { XF_MIX, XF_CURVE, XF_LEVEL };
enum { FT_LP, FT_HP, FT_BP, FT_NOTCH };
enum { XC_LIN, XC_EQPOWER };
enum { GR_TIME, GR_SIZE, GR_SHAPE, GR_PITCH, GR_DENSITY, GR_RANDOM, GR_FEEDBACK, GR_MIX, GR_DIR, GR_FREEZE };
enum { WAVE_SINE, WAVE_TRI, WAVE_SAW, WAVE_SQR, WAVE_CUSTOM };

typedef struct {
    bool     used;
    NodeType type;
    float    x, y;                  // canvas (world) position, UI only
    float    p[SYN_MAX_PARAMS];
    float    wt[SYN_WT_SIZE];       // custom waveform, -1..1
    int      input;                 // source node index, -1 = none
    int      input2;                // second input (XFADE B), -1 = none
    uint8_t  outputs;               // bit t = output for shape t
    int8_t   mod_src[SYN_MAX_PARAMS];   // step sequencer (0..7) driving each param, -1 = none

    // runtime (audio thread)
    float    out;                   // mono (left for REVERB)
    float    out_r;                 // right channel (stereo nodes: REVERB, GRAINS)
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
bool syn_connect(int src, int dst);                 // to input A; false if invalid / would loop
bool syn_connect_port(int src, int dst, int port);  // port 0 = input (A), 1 = input2 (B)
void syn_disconnect(int dst);
void syn_disconnect_port(int dst, int port);
int  syn_node_input(int node, int port);            // source on that port, -1 = none
void syn_set_param(int node, int k, float v);
void syn_set_wave_point(int node, int i, float v);
void syn_toggle_output(int node, int track);
void syn_clear_outputs(int node);
void syn_set_mod_src(int node, int k, int seq);    // -1 = none
float syn_clamp_param(const ParamDef *d, float v);  // keep v inside the parameter's range

// Step-sequencer outputs (S1..S8). A new value is written to every parameter
// assigned to `seq`, gliding there over slew_ms (0 = jump) on the audio thread.
enum { SLEW_LIN, SLEW_LOG };
#define SYN_MOD_SEQS 8
void syn_mod_target(int seq, float value, float slew_ms, int curve);
void syn_mod_reset(void);                           // forget the last values (playback restarts)
float syn_slew_shape(int curve, float t);           // 0..1 progress -> 0..1 of the way there
// Replace the whole patch (e.g. loading a slot). Copies used/type/x/y/p/wt/input/input2/outputs.
void syn_replace_all(const SynNode src[SYN_MAX_NODES]);

float syn_param_to_norm(const ParamDef *d, float v);
float syn_norm_to_param(const ParamDef *d, float u);

void syn_trigger(int track);                        // safe from any thread
void syn_render(int16_t *out, int nframes);         // audio thread, interleaved stereo L,R

// Microphone (MIC node). Call once per frame from the main thread: it turns the
// 3DS microphone on while a MIC node exists and off when none is left.
void syn_mic_poll(void);
void syn_mic_stop(void);
bool syn_mic_on(void);

bool syn_audio_start(void);   // false if the DSP couldn't start (dspfirm.cdc missing)
void syn_audio_stop(void);
bool syn_audio_ok(void);

// Step sequencers S1..S8: values, several targets, clamping, slew.
#include "t.h"

#include <stdio.h>
#include <string.h>

void suite_mod(void) {
    t_section("Step sequencers");
    syn_init();
    mod_init();
    t_clear_patch();
    int o = syn_add_node(NODE_OSC, 0, 0), o2 = syn_add_node(NODE_OSC, 0, 0), e = syn_add_node(NODE_ENV, 0, 0);
    CHECK(syn_nodes[o].mod_src[0] == -1 && syn_nodes[o].mod_src[SYN_MAX_PARAMS - 1] == -1, "a new node has no assignments");

    ModSeq *s1 = &mod_seqs[0];
    s1->trig = 1; s1->nsteps = 4; s1->min = 55; s1->max = 220;
    const float v[4] = { 0, 0.5f, 1, 0.25f }, want[5] = { 55, 137.5f, 220, 96.25f, 55 };
    for (int i = 0; i < 4; i++) s1->v[i] = v[i];
    syn_set_mod_src(o, OSC_FREQ, 0);
    syn_set_mod_src(o2, OSC_FREQ, 0);
    mod_reset();
    bool ok = true;
    for (int i = 0; i < 5; i++) {
        mod_on_pulse(1);
        ok &= fabsf(syn_nodes[o].p[OSC_FREQ] - want[i]) < 0.01f && syn_nodes[o2].p[OSC_FREQ] == syn_nodes[o].p[OSC_FREQ];
    }
    CHECK(ok, "S1 steps MIN + (MAX-MIN) x slider on both oscillators and loops after 4 steps");
    mod_on_pulse(0);
    CHECK(syn_nodes[o].p[OSC_FREQ] == 55, "other shapes don't advance it");
    CHECK(mod_seq_targets(0) == 2 && mod_seq_targets(1) == 0, "S1 drives 2 settings, S2 none");

    ModSeq *s3 = &mod_seqs[2];
    s3->trig = 2; s3->nsteps = 2; s3->min = -5; s3->max = 99999; s3->v[0] = 1; s3->v[1] = 0;
    syn_set_mod_src(e, ENV_D, 2);
    mod_on_pulse(2);
    float hi = syn_nodes[e].p[ENV_D];
    mod_on_pulse(2);
    float lo = syn_nodes[e].p[ENV_D];
    const ParamDef *d = &syn_classes[NODE_ENV].params[ENV_D];
    CHECK(hi == d->max && lo == d->min, "out-of-range values are clamped to the setting (%.0f..%.0f)", lo, hi);

    int cp = syn_add_copy(&syn_nodes[o], 0, 0);
    CHECK(syn_nodes[cp].mod_src[OSC_FREQ] == 0, "a pasted copy keeps its assignment");

    t_section("Slew");
    s1->nsteps = 2; s1->min = 100; s1->max = 1100; s1->v[0] = 0; s1->v[1] = 1;
    for (int c = 0; c < 2; c++) {
        const char *cn = c ? "LOG" : "LIN";
        mod_reset();
        s1->slew_ms = 100; s1->slew_curve = c;
        mod_on_pulse(1);
        CHECK(syn_nodes[o].p[OSC_FREQ] == 100, "%s: the first step after PLAY jumps", cn);
        mod_on_pulse(1);
        CHECK(syn_nodes[o].p[OSC_FREQ] == 100, "%s: the next step glides instead of jumping", cn);
        int done = 0;
        bool good = true;
        char got[96] = "";
        for (int ms = 25; ms <= 100; ms += 25) {
            int to = ms * SR / 1000;
            t_render(to - done);
            done = to;
            float now = syn_nodes[o].p[OSC_FREQ], w = 100 + 1000 * syn_slew_shape(c, ms / 100.0f);
            good &= fabsf(now - w) < 12 && syn_nodes[o2].p[OSC_FREQ] == now;
            snprintf(got + strlen(got), sizeof got - strlen(got), " %.0f", now);
        }
        CHECK(good, "%s 100 ms: Hz at 25/50/75/100 ms:%s", cn, got);
    }
    CHECK(syn_slew_shape(SLEW_LOG, 0.25f) > 0.6f && syn_slew_shape(SLEW_LIN, 0.25f) == 0.25f,
          "LOG covers most of the distance early (%.2f at 25%% of the time)", syn_slew_shape(SLEW_LOG, 0.25f));
    s1->slew_curve = SLEW_LIN;
    mod_on_pulse(1);                                        // toward 100 from 1100
    t_render(SR / 20);
    float mid = syn_nodes[o].p[OSC_FREQ];
    mod_on_pulse(1);                                        // turn back toward 1100 mid-glide
    t_render(32);
    CHECK(fabsf(mid - 600) < 12 && syn_nodes[o].p[OSC_FREQ] > mid && syn_nodes[o].p[OSC_FREQ] < mid + 20,
          "a new step mid-glide carries on from where it got to (%.0f -> %.0f Hz)", mid, syn_nodes[o].p[OSC_FREQ]);
    s1->slew_ms = 0;
    mod_on_pulse(1);
    CHECK(syn_nodes[o].p[OSC_FREQ] == 100, "SLEW 0 jumps straight away");
}

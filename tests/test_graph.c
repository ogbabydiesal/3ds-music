// Patching: evaluation order, fan-out, two-input XFADE, loop refusal, triggers.
#include "t.h"

#include <stdio.h>

void suite_graph(void) {
    t_section("XFADE and patching");
    syn_init();
    t_clear_patch();
    int a = syn_add_node(NODE_OSC, 0, 0), b = syn_add_node(NODE_OSC, 0, 0), x = syn_add_node(NODE_XFADE, 0, 0);
    syn_set_param(a, OSC_FREQ, 250); syn_set_param(b, OSC_FREQ, 400);
    syn_set_param(a, OSC_LEVEL, 0.5f); syn_set_param(b, OSC_LEVEL, 0.5f);
    CHECK(syn_connect_port(a, x, 0) && syn_connect_port(b, x, 1), "connect inputs A and B");
    syn_toggle_output(x, 0);
    const float mixes[] = { 0, 0.5f, 1 };
    for (int c = 0; c < 2; c++)
        for (int m = 0; m < 3; m++) {
            syn_set_param(x, XF_CURVE, c);
            syn_set_param(x, XF_MIX, mixes[m]);
            t_render(SR / 2);
            double ga = t_amp_at(250, SR / 4, SR / 2) / 0.5, gb = t_amp_at(400, SR / 4, SR / 2) / 0.5;
            double ea = c ? cos(mixes[m] * M_PI / 2) : 1 - mixes[m], eb = c ? sin(mixes[m] * M_PI / 2) : mixes[m];
            CHECK(fabs(ga - ea) < 0.02 && fabs(gb - eb) < 0.02, "%s MIX %3.0f%%: A x%.3f (want %.3f), B x%.3f (want %.3f)",
                  c ? "EQ PWR" : "LIN   ", mixes[m] * 100, ga, ea, gb, eb);
        }

    int r = syn_add_node(NODE_REVERB, 0, 0);
    syn_connect(b, r);
    syn_connect_port(r, x, 1);
    syn_set_param(x, XF_MIX, 1);
    t_render(SR / 2);
    CHECK(fabs(t_lr_corr(SR / 4, SR / 2)) < 0.9, "a stereo REVERB on input B stays stereo (L/R corr %.2f)", t_lr_corr(SR / 4, SR / 2));

    CHECK(!syn_connect_port(x, r, 0), "refused: XFADE -> REVERB, which already feeds XFADE (loop)");
    CHECK(!syn_connect_port(x, x, 1), "refused: XFADE into itself");
    CHECK(!syn_connect_port(a, b, 1), "refused: OSC has no input B");
    syn_disconnect_port(x, 1);
    CHECK(syn_node_input(x, 1) == -1 && syn_node_input(x, 0) == a, "unplugging B leaves A connected");
    syn_delete_node(a);
    CHECK(syn_node_input(x, 0) == -1, "deleting a node unplugs its cables");

    t_section("Fan-out (one output, many inputs)");
    t_clear_patch();
    int n = syn_add_node(NODE_NOISE, 0, 0), e = syn_add_node(NODE_ENV, 0, 0);
    int f1 = syn_add_node(NODE_FILTER, 0, 0), f2 = syn_add_node(NODE_FILTER, 0, 0), xf = syn_add_node(NODE_XFADE, 0, 0);
    syn_connect(n, e);
    CHECK(syn_connect(e, f1) && syn_connect(e, f2) && syn_node_input(f1, 0) == e && syn_node_input(f2, 0) == e,
          "ENV feeds two FILTERs at once");
    syn_connect_port(f1, xf, 0);
    syn_connect_port(f2, xf, 1);
    syn_set_param(f2, FLT_TYPE, FT_HP);
    syn_toggle_output(xf, 2);
    CHECK((syn_nodes[e].trig_mask & 4) && (syn_nodes[n].trig_mask & 4),
          "shape 3's pulses reach the ENV through both branches");
    t_render(256);
    syn_trigger(2);
    t_render(SR / 8);
    CHECK(t_rms(0, 0, SR / 20) > 0.02, "NOISE -> ENV -> 2 FILTERs -> XFADE plays (rms %.3f)", t_rms(0, 0, SR / 20));
    syn_trigger(1);
    t_render(SR / 8);
    CHECK(t_rms(0, 0, SR / 8) < 1e-4, "a different shape doesn't trigger it");
}

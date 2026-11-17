// Slot files: round trips, older formats, samples by name, moving old saves.
#include "t.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static int find_type(NodeType t) {
    for (int i = 0; i < SYN_MAX_NODES; i++) if (syn_nodes[i].used && syn_nodes[i].type == t) return i;
    return -1;
}

void suite_store(void) {
    t_section("Saving and loading");
    // Saves from before the rename: slot3 should be copied in; slot1 already exists and must not be overwritten.
    mkdir("old", 0777);
    mkdir("store", 0777);
    write_file("old/slot1.txt", "eucseq-slot 2\nbpm 99\n");
    write_file("old/slot3.txt", "eucseq-slot 2\nbpm 77\n");
    write_file("store/slot1.txt", "eucseq-slot 2\nbpm 140\n");
    syn_init();
    mod_init();
    store_init();
    FILE *f = fopen("store/slot3.txt", "r");
    char line[64] = "";
    if (f) { fgets(line, sizeof line, f); fgets(line, sizeof line, f); fclose(f); }
    CHECK(!strcmp(line, "bpm 77\n"), "an old eucseq slot is copied into the new folder");
    f = fopen("store/slot1.txt", "r");
    line[0] = 0;
    if (f) { fgets(line, sizeof line, f); fgets(line, sizeof line, f); fclose(f); }
    CHECK(!strcmp(line, "bpm 140\n"), "an existing slot is never overwritten by the copy");

    // Round trip: every kind of state a slot holds
    t_clear_patch();
    int sm = syn_add_node(NODE_SAMPLE, 10, 10), nz = syn_add_node(NODE_NOISE, 10, 50);
    int xf = syn_add_node(NODE_XFADE, 100, 30), os = syn_add_node(NODE_OSC, 10, 90);
    syn_connect_port(sm, xf, 0);
    syn_connect_port(nz, xf, 1);
    syn_toggle_output(xf, 1); syn_toggle_output(xf, 3);
    syn_set_param(xf, XF_MIX, 0.3f);
    syn_set_mod_src(xf, XF_MIX, 4);
    syn_set_param(os, OSC_WAVE, WAVE_CUSTOM);
    syn_set_wave_point(os, 5, -0.75f);
    syn_set_param(sm, SMP_FILE, samples_find("d_float"));
    mod_seqs[4].trig = 3; mod_seqs[4].nsteps = 5; mod_seqs[4].min = -2; mod_seqs[4].max = 7.5f; mod_seqs[4].v[2] = 0.125f;
    mod_seqs[4].slew_ms = 250; mod_seqs[4].slew_curve = SLEW_LOG;
    CHECK(store_save_slot(1), "save to slot 2");

    t_clear_patch();
    mod_init();
    CHECK(store_load_slot(1), "load slot 2");
    int lx = find_type(NODE_XFADE), lo = find_type(NODE_OSC), ls = find_type(NODE_SAMPLE);
    CHECK(lx >= 0 && syn_nodes[syn_node_input(lx, 0)].type == NODE_SAMPLE && syn_nodes[syn_node_input(lx, 1)].type == NODE_NOISE,
          "XFADE inputs A and B come back");
    CHECK(lx >= 0 && fabsf(syn_nodes[lx].p[XF_MIX] - 0.3f) < 1e-4 && syn_nodes[lx].mod_src[XF_MIX] == 4 &&
          syn_nodes[lx].outputs == ((1 << 1) | (1 << 3)), "settings, S assignment and outputs come back");
    CHECK(lo >= 0 && syn_nodes[lo].wt[5] == -0.75f, "a custom wave comes back");
    const ModSeq *m = &mod_seqs[4];
    CHECK(m->trig == 3 && m->nsteps == 5 && m->min == -2 && m->max == 7.5f && m->v[2] == 0.125f &&
          m->slew_ms == 250 && m->slew_curve == SLEW_LOG, "step sequencer S5, including SLEW, comes back");
    CHECK(ls >= 0 && !strcmp(samples_get((int)syn_nodes[ls].p[SMP_FILE])->name, "d_float"), "SAMPLE remembers its file");

    // Samples are saved by name: if the file list changes, the slot still finds its file
    f = fopen("store/slot2.txt", "r");
    char all[8192];
    size_t len = f ? fread(all, 1, sizeof all - 1, f) : 0;
    if (f) fclose(f);
    all[len] = 0;
    CHECK(strstr(all, "sample ") && strstr(all, " d_float\n"), "the slot file names the sample (\"sample N d_float\")");

    // A 0.4 slot (no input B, no slew) and a v1 slot (8 settings per node, no S assignments)
    char v04[2048] = "eucseq-slot 2\nbpm 120\nnode 0 2 0 0 -1 0";
    for (int k = 0; k < 10 + 16; k++) strcat(v04, " 0.5");
    for (int k = 0; k < 10; k++) strcat(v04, " -1");
    strcat(v04, "\nnode 1 1 0 0 0 1");
    for (int k = 0; k < 10 + 16; k++) strcat(v04, " 0.5");
    for (int k = 0; k < 10; k++) strcat(v04, " -1");
    strcat(v04, "\nseq 0 1 4 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    write_file("store/slot4.txt", v04);
    char v1[2048] = "eucseq-slot 1\nbpm 120\nnode 0 0 0 0 -1 0 1 0.6";      // NOISE: TYPE PINK, LEVEL 0.6
    for (int k = 0; k < 6 + 16; k++) strcat(v1, " 0");
    strcat(v1, "\nnode 1 1 0 0 0 1 1 100 0 50 0 1");                       // ENV fed by NOISE
    for (int k = 0; k < 2 + 16; k++) strcat(v1, " 0");
    strcat(v1, "\n");
    write_file("store/slot5.txt", v1);
    store_init();
    CHECK(store_load_slot(3) && syn_nodes[1].input == 0 && syn_nodes[1].input2 == -1 && mod_seqs[0].slew_ms == 0,
          "a 0.4 slot loads: input A kept, input B empty, no slew");
    CHECK(store_load_slot(4) && syn_nodes[0].type == NODE_NOISE && syn_nodes[0].p[NOISE_LEVEL] == 0.6f &&
          syn_nodes[1].input == 0 && syn_nodes[1].p[ENV_D] == 100 && syn_nodes[1].mod_src[0] == -1,
          "a v1 slot (8 settings per node) loads");
    CHECK(store_slot_used(0) && store_slot_used(1) && store_slot_used(2) && !store_slot_used(7),
          "slot list: 1-5 used, 8 empty");
}

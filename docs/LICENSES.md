# Licence audit

An audit of every algorithm, library and asset in **osc3ds** and **eucseq**, made to answer: *can this be released as freeware, or sold?*

> This is an engineering audit, not legal advice. For a paid release, have a lawyer review it, especially the trademark points.

## Short answer

- **Freeware: yes.** Everything in the apps is the project's own code, a standard published technique, or third-party code under a permissive licence. Ship [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with the build.
- **Paid: yes, after a few fixes.** None of the code is copyleft. The open items are about **names and branding**, not code. See [Before a paid release](#before-a-paid-release).

### Can the Beads algorithm be used? (the GRAINS node)

- **The code: yes.** The GRAINS node (first called BEADS) was written for this project from the public Beads *manual*. No Mutable Instruments source code was read or copied. Even if it had been, Beads' firmware is in pichenettes/eurorack, where the STM32 module code is under the **MIT licence**, which allows paid use if its notice is included.
- **The name: changed to GRAINS.** The eurorack repository's README says *Mutable Instruments* is a registered trademark. It asks that derivative works not use that name, and recommends not reusing the original module names. Its example renames "Clouds" to "Particle Generator". So the node is now called **GRAINS**. The attribution, *"an implementation of the Beads granular algorithm by Emilie Gillet / Mutable Instruments … Not affiliated with Mutable Instruments"*, sits on the **Settings → CREDITS** page and in the docs. That is factual, credit-giving use of the name; the module name isn't used as the product or node name.

## Algorithms

| Part | Where | Origin | Licence / status | Risk |
|---|---|---|---|---|
| **REVERB** (JCRev) | `synth.c` `process_reverb` | Port of STK's `JCRev` (Perry Cook, Gary Scavone), after John Chowning's design and Schroeder's allpass/comb reverberator (1960s) | **STK licence (MIT-style)**: commercial use allowed, **keep the copyright and permission notice**. Included in THIRD_PARTY_NOTICES.md. Any patents on the 1960s designs have long expired. | Low |
| **GRAINS** granular | `synth.c` `process_grains` | Own implementation of granular synthesis (Gabor, Xenakis, Roads), with features modeled on the Beads manual | Own code. Granular synthesis is a decades-old public technique. Renamed from BEADS; Beads is credited, not used as a name. | Low |
| Oscillators | `synth.c` `process_osc` | Sine, triangle, saw and square formulas; **PolyBLEP** anti-aliasing (Välimäki and colleagues, published around 2007) | Textbook DSP, own implementation. We know of no patent on PolyBLEP. | Low |
| Custom wavetable | `synth.c` | 16-point linear-interpolated table | Own | None |
| White noise | `synth.c` `white` | xorshift32 (George Marsaglia, 2003) | Public algorithm | None |
| Pink noise | `synth.c` `process_noise` | Paul Kellett's "economy" pink filter, posted to music-dsp in 2002: three one-pole filters | No licence stated. It's a short formula with coefficients, shared publicly for general use and used everywhere. If you want zero doubt, swap it for a Voss–McCartney generator written from scratch. | Very low |
| Brown noise | `synth.c` | Leaky integrator | Own | None |
| Envelope (ADSR + LENGTH) | `synth.c` | Linear attack, exponential decay and release | Own | None |
| Euclidean rhythms | `main.c` `track_rebuild` | Even pulse spread via `(i·k) mod n < k`, after Toussaint's 2005 paper on Euclidean rhythms | Own one-line implementation of a published idea | None |
| Step sequencers S1–S8, song mode, save slots | `modseq.c`, `store.c` | Own | Own | None |
| OSC encoding | `common/osc.c` | OSC 1.0 specification (CNMAT, UC Berkeley) | Open specification, free to implement | None |
| Stereoscopic 3D | `sequencer/source/main.c` | Off-axis (asymmetric frustum) stereo projection | Standard graphics math | None |
| Grain envelopes, soft limiter, pan law | `synth.c` | Generic DSP | Own | None |

## Libraries linked into the apps

| Library | Licence | What you must do |
|---|---|---|
| libctru | zlib | Nothing for binaries. libctru is a clean-room library, not Nintendo's SDK. |
| citro2d, citro3d | zlib | Nothing for binaries. |
| newlib (C library) | Several BSD-style licences | Some ask for their notices in your documentation; THIRD_PARTY_NOTICES.md points to them. |
| libgcc / libm | GCC Runtime Library Exception | Nothing; closed-source and paid programs are allowed. |

Tools used only to build (devkitARM GCC, 3dslink, mkbcfnt, makerom, the macOS screenshot renderer) don't put their licences on what they produce.

## Assets and data

| Asset | Status |
|---|---|
| **Arial** (`sequencer/romfs/arial.bcfnt`) | Monotype's font, licensed with macOS. **It must not be redistributed.** It's git-ignored, and `make release` leaves it out. The release `.3dsx` files were checked and contain no font. Only your own `make` / `make send` builds include it. |
| 3DS system font | Read from the console at runtime and never shipped. Fine. |
| **App icon** | Both apps use **libctru's `default_icon.png`**, the devkitPro logo. **Replace it with your own `icon.png`** before release. |
| `dspfirm.cdc` | Nintendo's DSP firmware. It's never shipped: each user dumps their own with DSP1. Keep it that way. |
| Screenshots in `docs/` | Rendered from this app's UI. Text drawn with a font in an image is normal use of the font. |
| Max patch, Python OSC monitor | Own. The monitor uses only Python's standard library. |

## Before a paid release

1. ~~Rename BEADS~~ Done: it's **GRAINS**, with attribution on the CREDITS page. Saved slots weren't affected, since nodes are stored by type number.
2. **Replace the default icon** with your own art (48×48 `icon.png` next to each Makefile).
3. **Ship THIRD_PARTY_NOTICES.md** with every build. `make release` now copies it into `dist/`, and the app's **Settings → CREDITS** page lists the attributions and the STK copyright.
4. **Add a licence for your own code.** The repo currently has no `LICENSE` file, so all rights are reserved by default, which is fine for selling. Choose a licence if you want others to be able to reuse the source.
5. **Check the names.** Avoid Nintendo trademarks in a product name ("osc3ds" uses "3DS"), and don't describe the app as official or endorsed. "Runs on Nintendo 3DS homebrew" is a factual description.
6. **Where to sell.** The 3DS eShop is closed, so homebrew is sold directly, for example on itch.io or Gumroad. Nothing in this code stops that. Running it needs a homebrew-enabled console; describe that requirement plainly.

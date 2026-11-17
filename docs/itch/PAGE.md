# itch.io page kit for nodeTrix

Everything to paste into itch.io, plus the setup steps. The files to upload are made by:

```bash
make itch
```

This writes `dist/nodeTrix-0.4.0.zip`, containing `nodeTrix.3dsx`, `INSTALL.txt`, `LICENSE` and `THIRD_PARTY_NOTICES.md`. Change the version with `make itch NODETRIX_VERSION=0.4.1`.

## 1. Account and payments (one time)

1. Create an itch.io account. Under **Settings → Payments**, connect **PayPal** or **Stripe** for payouts.
2. Under **Settings → Payments**, choose the **"Collected by itch.io, paid later"** payment mode. itch.io is then the seller of record and handles VAT and US sales tax.
3. Complete the **tax interview**: a W-9 if you're a US person, a W-8BEN otherwise.
4. Optionally, set your default **revenue share**. The default is 10% to itch.io.

## 2. Create the project

Go to **Dashboard → Create new project**:

| Field | Value |
|---|---|
| Title | `nodeTrix` |
| Project URL | `nodetrix` (gives `yourname.itch.io/nodetrix`) |
| Short description / tagline | `A 3D Euclidean sequencer and modular synth for Nintendo 3DS homebrew` |
| Classification | **Tools** |
| Kind of project | **Downloadable** |
| Release status | **Released** |
| Pricing | **$5.00 or more**, with "Let people pay more" on |
| Uploads | `nodeTrix-0.4.0.zip`. Leave Windows/macOS/Linux/Android **unticked**; it's a 3DS file. |
| Cover image | `docs/itch/cover.png` (630×500) |
| Screenshots | See [Screenshots](#4-screenshots) |
| Genre | (Tools have no genre) |
| Tags | `music`, `sequencer`, `synthesizer`, `homebrew`, `3ds`, `osc`, `euclidean-rhythm`, `granular`, `audio`, `stereoscopic` |
| AI disclosure | itch.io asks whether generative AI was used. This project's code, docs and images were made with Claude (an AI model), so answer **yes** and describe the use honestly. |
| Community | Comments on, if you want questions on the page |
| Visibility | Draft first. Check the page, then switch it to **Public**. |

## 3. Page text (paste into the description)

---

**nodeTrix** turns a Nintendo 3DS into a polyrhythmic groovebox you can see in 3D.

Five wireframe shapes (a cube, a sphere, a pyramid, a diamond and a cylinder) orbit a ring on the stereoscopic top screen. A star sweeps around the ring, and every time it reaches a shape, that shape fires. That plays the synth patches you build on the touchscreen, sends OSC to your computer, or both.

**Sequencer**
- 5 tracks with Euclidean rhythms: DIVS, PULSES and ROTATE for each, plus mute
- True stereoscopic 3D with the 3D slider, and an EXPAND view
- 8 save slots and a song mode that chains patterns bar by bar

**Modular synth**
- Patch nodes together on a zoomable canvas: oscillators (sine, triangle, saw, square, or a wave you draw), noise (white, pink, brown) and ADSR envelopes
- **SAMPLE**: play your own .wav files on every hit, with pitch, start/end and loop
- **MIC**: the 3DS microphone as a live sound source
- **FILTER**: a resonant low-pass, high-pass, band-pass or notch filter
- **DELAY**: up to 2 s of feedback delay with a filter in the loop and tape-style time glides
- **CHORUS**: a one-knob analog-style chorus with a depth switch, for that watery early-'90s clean tone
- **REVERB**: a port of John Chowning's classic JCRev reverb from Stanford's STK
- **GRAINS**: a granular texture processor, an implementation of the Mutable Instruments Beads algorithm. It records, freezes, pitch-shifts, reverses and scatters grains in stereo.
- Box select, copy and paste, and patches saved with your patterns

**8 step sequencers**
- S1–S8 each step through up to 16 sliders on a shape's pulses
- Assign any synth setting to any sequencer, right in the instrument's settings

**OSC out**
- `/shape_N/pulse` on every hit, plus optional gyro and accelerometer streams
- Works with Max/MSP, Ableton (via Max for Live), TouchDesigner, Pure Data, VCV Rack, Bitwig, Reaper, Resolume and more

**Requirements**
- A Nintendo 3DS / 2DS family console with **custom firmware and the Homebrew Launcher**
- For sound: the DSP firmware file `sdmc:/3ds/dspfirm.cdc`, made once with the **DSP1** homebrew. nodeTrix doesn't include Nintendo files.
- OSC needs the 3DS and your computer on the same Wi-Fi network

**Install:** copy `nodeTrix.3dsx` to `/3ds/` on your SD card and launch it from the Homebrew Launcher. `INSTALL.txt` in the download has the details.

**Open source.** nodeTrix is MIT-licensed; the full source and manual are on GitHub (github.com/ogbabydiesal/nodeTrix). Buying it here gets you a ready-to-run build and supports development.

**Privacy:** nodeTrix collects no data and contacts no servers. It only sends OSC on your own network, to the address you set. The microphone is used only while you have a MIC node in your patch, and its audio never leaves the console.

*nodeTrix is independent homebrew, not affiliated with or endorsed by Nintendo or Mutable Instruments. Nintendo 3DS is a trademark of Nintendo. Modifying a console is at your own risk, and the software comes with no warranty. Credits are in Settings → CREDITS.*

---

## 4. Screenshots

Upload these (itch.io shows them in the sidebar):

1. `docs/screenshots/eucseq-tracks-playing.jpg`: the 3D top screen with shapes on the ring
2. `docs/screenshots/synth-3-two-instruments.png`: the patch canvas
3. `docs/screenshots/synth-9-grains.png`: OSC → ENV → GRAINS → REVERB
4. `docs/screenshots/synth-10-grains-settings.png`: GRAINS settings
5. `docs/screenshots/steps-1-lane.png`: the STEPS page
6. `docs/screenshots/synth-11-sample-filter-delay-mic-chorus.png`: the new sound sources and effects
7. `docs/screenshots/store-2-song.png`: song mode

A short video of the top screen (filmed with a phone; the 3D effect doesn't survive screen capture) also works well. itch.io takes a YouTube or Vimeo link.

## 5. Before you publish

- [ ] `make itch` built the zip, and you tested that `.3dsx` on the console
- [ ] The page is a draft, viewed with **View page**
- [ ] Payment mode is "Collected by itch.io", and the tax interview is done
- [ ] The README's "coming to itch.io" line is updated with the real link
- [ ] A matching GitHub release (v0.4.0) is published, if you want the free build there too

## Updating later

Build a new zip with a higher `NODETRIX_VERSION` and upload it in place of the old file. Buyers get it from their library. To upload from the command line, itch.io's **butler** tool can push new builds.

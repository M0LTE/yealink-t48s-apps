# Demo

Real-time graphics and a chiptune on a desk phone, in the old demo style: a loop of about two and a half minutes, drawn and played by a 500 MHz ARM1176 with no GPU and no usable FPU. The tune is made as it plays; there is not a sample in the package. Issue [#13](https://github.com/M0LTE/yealink-t48s-apps/issues/13).

| Key | |
|---|---|
| `*` | show or hide the frame rate |
| left, right | previous, next part |
| any other key | leave |

The volume keys set how loud it is. On firmware without sound for apps (before API level 4) it runs the same, silent. The phone's lamps keep time with the tune on firmware that lends them to apps (API level 5).

![the knot](screenshots/knot.png)
![the tunnel](screenshots/tunnel.png)
![the end](screenshots/outro.png)

## The parts

| Part | What it shows |
|---|---|
| stars | a starfield with sub-pixel stars that streak at speed, and three title cards |
| gem | a flat-shaded solid lit by two lamps, every edge smoothed |
| plasma | five sines through a turning palette, worked out on a grid and stepped between |
| knot | a Gouraud-shaded torus knot of 3584 triangles |
| roto | a rotozoomer |
| tunnel | a textured tunnel with the eye free to look about, lit from ahead |
| outro | a mirror (environment-mapped) torus over a sine scroller |

## What the phone manages

The screen refreshes 67.8 times a second, so a part runs at 67.8 frames a second or at 33.9. Writing memory is the limit: about 160 MB a second, so filling 800 x 480 once takes 10 of the 14.7 ms in a refresh. On the phone, with the tune playing (2026-11-25; draw times from `make bench`, frame rates from the app's own log on firmware level 4, flipping):

| Part | Drawn at | Draw time | Frames a second |
|---|---|---|---|
| stars | 800 x 480, only what moves | 4.0 ms | 67.8 |
| gem | 800 x 480, only what moves | 9.7 ms | 67.6 |
| plasma | 800 x 384 | 8.9 ms | 67.8 |
| knot | 800 x 480, only what moves | 20.0 ms | 34.0 |
| roto | 400 x 192, doubled | 10.6 ms | 67.7 |
| tunnel | 800 x 384 | 23.5 ms | 34.0 |
| outro | 800 x 480, only what moves | 23.6 ms | 33.9 |

Every part keeps the pace it asks for. The tune costs 1.2% of the processor by itself (144 s of sound made in 1.7 s), and 1.4 to 2.0% as the app's log counts it, with the drawing pushing it out of the cache; a whole loop played with no gap. That run was silent (the audio service's test mode keeps the amplifier off): the frame rates and the sound sent to the codec were measured, and nobody has yet heard it from the phone's loudspeaker or watched it on the glass.

## How it is drawn

- **Fixed point everywhere.** Floats are used only to make tables at start.
- **Solids** (`r3d.c`): screen positions carry four bits below the pixel and every edge is stepped from where it really starts, so a slow turn glides instead of snapping. Shading is a plane over each triangle evaluated at pixel centres. Triangles are sorted far to near. After each is filled, its outline edges get one more row of pixels mixed in by coverage, which is what smooths the silhouette.
- **Only what changed is redrawn.** The sky is black, so a solid part clears the box its solid covered last time in this buffer, rubs out last time's stars, and draws both again: a fraction of the screen.
- **Plasma, rotozoomer and tunnel** (`fx.c`) are one renderer: the effect is worked out exactly at the corners of 16 x 16 cells and stepped in straight lines between, cell by cell so texture reads stay in the cache.
- **The scroller** takes each screen column from a strip of text made once from the phone's own font, moved by a sine with sub-pixel mixing in y. It moves a whole number of pixels each refresh.

## Frames and time

`fe.c` is the only file that knows how a frame reaches the glass. With firmware API level 3 it draws into the shell's flip buffers and each frame is shown whole at a refresh of the screen (67.8 a second on the T48S): no tearing, no copy. On older firmware it falls back to the shared buffer and DAMAGE, which tears.

Each part asks for a pace: every refresh, or every second one. A part that cannot keep its pace is stepped down for the rest of its run, as an even 34 frames a second looks better than an uneven 50.

The timeline (`demo.c`) is laid out in bars at 125 beats a minute, and its clock moves by whole refreshes of the screen: that is what keeps motion even. The tune follows the picture, not the other way round.

## The tune

An original tune in A minor, written for this demo (`tune.c`): a block of bars for each part, and three blocks that take turns under the outro for as long as its text runs.

- **The player** (`music.c`) works like a tracker. Six channels: four play notes, two play drums. A tick is 20 ms, a row six ticks (a sixteenth note), a bar sixteen rows. Each tick the envelopes, vibrato, pulse widths and chords move (a chord is one voice stepping through its three notes fifty times a second); between ticks it is one short loop a voice.
- **The voices** are pulse of any width, saw and triangle. Saw comes from tables that hold only the harmonics under 6.8 kHz for the octave being played, so nothing folds back as a whistle at 16000 samples a second, and a pulse is one saw minus the same saw a little later. Drums are a sine that falls in pitch, plus noise. An echo three rows long sits behind the leads.
- **For a small loudspeaker:** the bass is written an octave up and is rich in harmonics, since the fundamental would not be heard; nothing under 100 Hz is sent; the top is soft; peaks stay under full scale and the odd one over the knee is squeezed, not clipped.
- **In time** (`sound.c`): sound written now is heard about 65 ms later, and a frame drawn now is seen one or two refreshes later. A thread makes each tick of sound, works out when it will be heard and where the picture will be by then, and makes the tick a few samples shorter or longer to stay with it: the tempo gives a little, the pitch does not. Stepping to another part, or coming back from Home, the tune jumps.
- **The picture listens:** parts are told the kick, the snare and the bass, read from the score (so it is the same with no sound). The stars and solids flare on the kick, the plasma's colours jump, the rotozoomer is knocked back, and the wave the scroller rides swells with each bass note.
- **So do the lamps** (`lamps.c`): the Speaker key's lamp flashes on each kick and glows with the bass, the Headset key's on each snare, the red Mute lamp between them swells and fades with the chords, a fill runs down the three a lamp a hit, and a crash lights them all and flashes the red lamp at the top of the case. Read from the score too, and set a frame behind the drawing, so they change as the picture does.

`make wav` writes the whole tune to `build/demo.wav` with the same player, sample for sample, to listen to on a PC.

## Working on it

```
make check                     # the simulator steps through every part
make bench                     # build/arm/demo-bench: draw times and the synth's cost, without a shell; run it on the phone
make wav                       # build/demo.wav: the tune, from the player the phone runs
DEMO_AT=47000 make shot        # a picture from 47 s into the loop
DEMO_DRAW_MS=20 ...            # every frame takes at least this long to draw: for trying the pacing
```

With the overlay on, each part's frame rate and draw time are also written to `fps.log` in the app's data folder when the part ends, with what the tune cost and any gaps in it.

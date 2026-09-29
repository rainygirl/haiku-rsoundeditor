<img src="docs/icon.png" width="64" align="left" alt="">

# R Sound Editor

A recorder and small waveform editor for Haiku OS.

[한국어](README.ko.md)

![Recording, with the waveform scrolling live](docs/recording.png)

Haiku OS's stock SoundRecorder renders its scope from a finished `BMediaTrack`, so the waveform only appears once you stop recording — you cannot see whether the microphone is picking anything up while a take is running. Here the trace scrolls live; once stopped, the same view shows the whole take and can be dragged to select a region to edit.

The recording is held in memory as float frames, so every edit (cut, gain, speed) is lossless until the WAV is written.

## Requirements

Haiku OS (x86 or x86_64). Build it on the machine you are going to run it on — no cross-compiler needed.

Also builds and runs on arm64, cross-compiled against the Haiku kits; it needs nothing outside them.

## Install

```sh
./install.sh
```

This compiles it, puts the binary in `~/config/non-packaged/apps/`, adds it to **Deskbar → Applications**, and puts a link on the Desktop.

```sh
./install.sh --build-only   # compile in place, install nothing
./install.sh --uninstall    # remove it again
```

## Using it

- Live scrolling waveform while recording
- Mono / stereo, or **Auto** — listens for the first 0.5 s and commits to whichever the input actually is
- Playback with elapsed/total time and a playhead that tracks it
- Drag on the waveform to select a region; delete it, trim to it, or silence it (`Delete` or `Backspace` deletes the selection)
- Volume ±3 dB, normalize
- Speed ×1.25 / ×0.8 — the document itself is resampled, so the change survives export
- Save as WAV through a file panel
- English and Korean UI, switchable at runtime from the **Language** menu; starts in the system language

## License

MIT

## AI disclosure

This program was written with Claude.

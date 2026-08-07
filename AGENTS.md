# R Sound Editor — development notes

## Everything runs on the window thread

The capture callback fills a lock-free single-producer ring; the window
redraws from it on a 33 ms `BMessageRunner` tick.

There used to be a render thread inside `WaveView` that locked the window and
invalidated. That put the view, the sample ring and the document in reach of
a thread with a lifetime of its own, and the application died twice with
segment violations in it — once alongside a second fault in the window
thread, which is what a shared object being torn down under a reader looks
like. Removing the thread removed the whole class of failure.

`BWindow` does not deliver `B_PULSE` to its own `MessageReceived` — pulses go
to views that asked for them — hence the explicit runner.

## AudioDoc needs a lock

The buffer is written by the capture callback on a media thread and read by
the window thread (drawing, editing, saving) and by the playback callback on
a third. `Append()` reallocs, so a reader holding the old pointer reads freed
memory — observed as a segment violation inside `WaveView::Draw` after about
a minute of recording, once the buffer had grown enough for realloc to
relocate it.

`SetLive(true)` has to be set *before* `BMediaRecorder::Start()`: the capture
callback can deliver its first buffer, and `_DecideLayout()` can free and
reallocate the document, the instant the recorder starts.

## Four defects in the platform, fixed in the VAIO P patch set

Recording produced silence with no error anywhere. The fixes live in
`haiku-sony-vaio-p-patch`, not here, but they are what this app depends on:

1. `BMediaRecorder::Start()` treated "is a time source" and "is the node we
   pull from" as alternatives. Every multi_audio device node is both, so the
   node stayed in `B_STOPPED` and every captured buffer was discarded.
2. `Stop()` did not stop the producer and `Disconnect()` returned early when
   `Stop()` failed, leaving the capture device occupied until the media
   server was restarted.
3. `set_global_format()` overwrote the supported-rate/format masks with the
   single chosen value, so `get_description()` then reported that one value
   as the entire capability list.
4. Capture amplifiers were left at 0 dB, which for an electret internal
   microphone means the ADC only sees the noise floor.

## Playback

`FillPlayback()` uses the channel count the player actually negotiated, not
the document's — the mixer is free to hand back something else, and reading
the buffer with the wrong stride writes interleaved noise.

`SetVolume(1.0)` is set explicitly on each new player: the mixer hands every
new one a volume of its own and it has been observed at zero, which plays a
perfectly correct buffer to nobody.

## Do not overwrite a running binary

`install.sh` quits the app and refuses to install if it is still running.
Replacing the file under a live process invalidates its code pages and it
then dies in places that have nothing to do with the real cause.

## A symptom that was not ours

A pure sine played back on this hardware ticks audibly, with correct buffer
counts and no driver-side underrun. It does so with the stock driver too and
music masks it completely — a pre-existing playback glitch (the log's
`DMA position ... broken, switching to LPIB` is the likely culprit), not
something this app or the patch set causes.

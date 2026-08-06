/*
 * R Sound Editor -- record with a live waveform, then play back and edit.
 *
 * Haiku's stock SoundRecorder renders its scope from a finished BMediaTrack,
 * so the waveform only appears once recording has stopped. Here the capture
 * callback fills a lock-free ring that a render thread drains, so the trace
 * scrolls while recording; once stopped, the same view shows the whole take
 * and can be dragged to select a region to edit.
 *
 * The recording is held in memory as float frames, which keeps the edits
 * (cut, gain, speed) straightforward and lossless until the file is written.
 *
 * Distributed under the terms of the MIT License.
 */

#include <Alert.h>
#include <Autolock.h>
#include <Application.h>
#include <Button.h>
#include <Entry.h>
#include <Locker.h>
#include <FilePanel.h>
#include <LocaleRoster.h>
#include <FindDirectory.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Bitmap.h>
#include <ControlLook.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <SoundPlayer.h>
#include <String.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const uint32 kMsgToggleRecord		= 'rec ';
static const uint32 kMsgTogglePlay			= 'play';
static const uint32 kMsgPlaybackFinished	= 'pfin';
static const uint32 kMsgMode				= 'mode';

static const uint32 kMsgCut					= 'ecut';
static const uint32 kMsgTrim				= 'etrm';
static const uint32 kMsgSilence				= 'esil';
static const uint32 kMsgSelectAll			= 'esla';
static const uint32 kMsgSelectNone			= 'esln';
static const uint32 kMsgLouder				= 'evup';
static const uint32 kMsgQuieter				= 'evdn';
static const uint32 kMsgNormalize			= 'enrm';
static const uint32 kMsgFaster				= 'espu';
static const uint32 kMsgSlower				= 'espd';
static const uint32 kMsgSave				= 'save';
static const uint32 kMsgTick				= 'tick';
static const uint32 kMsgLanguage			= 'lang';

enum channel_mode {
	kChannelAuto = 0,
	kChannelMono,
	kChannelStereo
};

// How long to listen, in Auto mode, before committing to mono or stereo.
static const double kAutoDetectSeconds = 0.5;

// Live scope history: far more than the few hundred pixels ever drawn.
static const int32 kRingSize = 131072;


// Menu and status text in both languages.
//
// The locale kit would be the idiomatic route, but it wants catalog files
// built and installed alongside the binary; this app is a single translation
// unit that gets copied around on its own, so the strings live here and the
// language can also be switched at runtime from the menu.
enum string_id {
	kStrFile = 0, kStrSave, kStrQuit,
	kStrEdit, kStrSelectAll, kStrSelectNone, kStrDelete, kStrTrim, kStrSilence,
	kStrVolume, kStrLouder, kStrQuieter, kStrNormalize,
	kStrSpeed, kStrFaster, kStrSlower,
	kStrLanguage, kStrEnglish, kStrKorean,
	kStrChannels, kStrAuto, kStrMono, kStrStereo,
	kStrRecord, kStrStopRecording, kStrPlay, kStrStop,
	kStrReady, kStrNoInput, kStrDetecting,
	kStrRecordingFmt, kStrPlayingFmt, kStrSelectionFmt, kStrHintFmt,
	kStrCannotRecord, kStrCannotPlay, kStrSaved, kStrCannotSave,
	kStrSavePanel, kStrDefaultName,
	kStringCount
};

static const char* const kStringsEn[kStringCount] = {
	"File", "Save as WAV...", "Quit",
	"Edit", "Select all", "Select none", "Delete selection",
	"Trim to selection", "Silence selection",
	"Volume", "Louder (+3 dB)", "Quieter (-3 dB)", "Normalize",
	"Speed", "Faster (x1.25)", "Slower (x0.8)",
	"Language", "English", "Korean",
	"Channels:", "Auto", "Mono", "Stereo",
	"Record", "Stop recording", "Play", "Stop",
	"ready", "no audio input device", "detecting...",
	"recording %.1f s   %" B_PRIu32 " Hz / %s   peak %.1f dBFS   rms %.1f dBFS",
	"playing  %.2f / %.2f s   %" B_PRId32 " ch   %" B_PRId32 " buffers"
		"   out peak %.1f / rms %.1f dBFS   vol %.2f",
	"%.2f s  %s  |  selection %.2f-%.2f s  (%.2f s)",
	"%.2f s  %s  |  drag on the waveform to select a region",
	"Could not start recording: ", "Could not play: ",
	"Saved ", "Could not save: ",
	"Save recording", "Recording.wav"
};

static const char* const kStringsKo[kStringCount] = {
	"파일", "WAV로 저장...", "끝내기",
	"편집", "모두 선택", "선택 해제", "선택 구간 삭제",
	"선택 구간만 남기기", "선택 구간 무음화",
	"음량", "키우기 (+3 dB)", "줄이기 (-3 dB)", "정규화",
	"속도", "빠르게 (x1.25)", "느리게 (x0.8)",
	"언어", "English", "한국어",
	"채널:", "자동", "모노", "스테레오",
	"녹음", "녹음 중지", "재생", "정지",
	"준비됨", "오디오 입력 장치 없음", "판별 중...",
	"녹음 중 %.1f초   %" B_PRIu32 " Hz / %s   피크 %.1f dBFS   rms %.1f dBFS",
	"재생 중  %.2f / %.2f초   %" B_PRId32 "ch   %" B_PRId32 " 버퍼"
		"   피크 %.1f / rms %.1f dBFS   음량 %.2f",
	"%.2f초  %s  |  선택 %.2f-%.2f초  (%.2f초)",
	"%.2f초  %s  |  파형을 드래그해 구간을 선택하세요",
	"녹음을 시작할 수 없습니다: ", "재생할 수 없습니다: ",
	"저장됨: ", "저장할 수 없습니다: ",
	"녹음 저장", "Recording.wav"
};

static const char* const* sStrings = kStringsEn;

static inline const char*
T(string_id id)
{
	return sStrings[id];
}


// Buttons carry glyphs rather than words: at this size a filled circle and a
// triangle read faster than "Record" and "Play", and they stay legible when
// the label has to change to a stop square mid-take.
static BBitmap*
make_icon(uint32 what, rgb_color color)
{
	const float kSize = 16;
	BBitmap* bitmap = new BBitmap(BRect(0, 0, kSize - 1, kSize - 1),
		B_RGBA32, true);
	if (bitmap->InitCheck() != B_OK) {
		delete bitmap;
		return NULL;
	}

	BView* view = new BView(bitmap->Bounds(), "icon", B_FOLLOW_NONE, 0);
	bitmap->AddChild(view);
	if (!bitmap->Lock()) {
		delete bitmap;
		return NULL;
	}

	view->SetHighColor(0, 0, 0, 0);
	view->FillRect(view->Bounds());
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetHighColor(color);

	switch (what) {
		case 'circ':	// record
			view->FillEllipse(BRect(2, 2, kSize - 3, kSize - 3));
			break;

		case 'tria':	// play
		{
			BPoint points[3] = {
				BPoint(3, 2), BPoint(3, kSize - 3), BPoint(kSize - 2, kSize / 2)
			};
			view->FillPolygon(points, 3);
			break;
		}

		case 'squa':	// stop
			view->FillRect(BRect(3, 3, kSize - 4, kSize - 4));
			break;
	}

	view->Sync();
	bitmap->Unlock();
	bitmap->RemoveChild(view);
	delete view;
	return bitmap;
}


// Single-producer/single-consumer ring. The capture callback runs on a media
// thread and the render loop on its own; neither may block the other, so
// there is no lock -- the writer only advances fWritePos after storing.
class SampleRing {
public:
	SampleRing()
		:
		fWritePos(0)
	{
		memset(fSamples, 0, sizeof(fSamples));
	}

	void Push(float value)
	{
		int32 pos = fWritePos;
		fSamples[pos & (kRingSize - 1)] = value;
		fWritePos = pos + 1;
	}

	void ReadRecent(float* dest, int32 count) const
	{
		int32 end = fWritePos;
		for (int32 i = 0; i < count; i++) {
			int32 index = end - (count - i);
			dest[i] = index < 0 ? 0.0f : fSamples[index & (kRingSize - 1)];
		}
	}

private:
			float			fSamples[kRingSize];
	volatile int32			fWritePos;
};


// The recording itself: interleaved float frames, so every edit is a plain
// array operation and nothing is quantised until the WAV is written.
class AudioDoc {
public:
	AudioDoc()
		:
		fLock("audio doc"),
		fData(NULL),
		fFrames(0),
		fCapacity(0),
		fChannels(1),
		fRate(48000)
	{
	}

	~AudioDoc()
	{
		free(fData);
	}

	// The buffer is written by the capture callback on a media thread and
	// read by the window thread (drawing, editing, saving) and by the
	// playback callback on a third. Append() reallocs, so a reader holding
	// the old pointer reads freed memory -- observed as a segment violation
	// inside WaveView::Draw after about a minute of recording, once the
	// buffer had grown enough for realloc to relocate it.
	//
	// BLocker is recursive, so a caller may hold Locker() across several
	// calls that each lock again.
	BLocker* Locker() const { return &fLock; }

	void Reset(int32 channels, uint32 rate)
	{
		BAutolock lock(fLock);
		free(fData);
		fData = NULL;
		fFrames = 0;
		fCapacity = 0;
		fChannels = channels;
		fRate = rate;
	}

	bool Append(const float* frame)
	{
		BAutolock lock(fLock);
		if (fFrames >= fCapacity) {
			int64 capacity = fCapacity == 0 ? 48000 : fCapacity * 2;
			float* grown = (float*)realloc(fData,
				capacity * fChannels * sizeof(float));
			if (grown == NULL)
				return false;
			fData = grown;
			fCapacity = capacity;
		}
		for (int32 c = 0; c < fChannels; c++)
			fData[fFrames * fChannels + c] = frame[c];
		fFrames++;
		return true;
	}

	float*	Data() const { return fData; }
	int64	Frames() const { return fFrames; }
	int32	Channels() const { return fChannels; }
	uint32	Rate() const { return fRate; }
	double	Duration() const
				{ return fRate > 0 ? (double)fFrames / fRate : 0; }

	void Remove(int64 from, int64 to)
	{
		BAutolock lock(fLock);
		if (fData == NULL || to <= from)
			return;
		if (from < 0)
			from = 0;
		if (to > fFrames)
			to = fFrames;
		int64 tail = fFrames - to;
		if (tail > 0) {
			memmove(fData + from * fChannels, fData + to * fChannels,
				tail * fChannels * sizeof(float));
		}
		fFrames = from + tail;
	}

	void KeepOnly(int64 from, int64 to)
	{
		BAutolock lock(fLock);
		if (to <= from)
			return;
		Remove(to, fFrames);
		Remove(0, from);
	}

	void Scale(int64 from, int64 to, float factor)
	{
		BAutolock lock(fLock);
		if (fData == NULL)
			return;
		if (from < 0)
			from = 0;
		if (to > fFrames)
			to = fFrames;
		for (int64 i = from * fChannels; i < to * fChannels; i++) {
			float v = fData[i] * factor;
			fData[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
		}
	}

	float PeakIn(int64 from, int64 to) const
	{
		BAutolock lock(fLock);
		if (fData == NULL)
			return 0;
		if (from < 0)
			from = 0;
		if (to > fFrames)
			to = fFrames;
		float peak = 0;
		for (int64 i = from * fChannels; i < to * fChannels; i++) {
			float a = (float)fabs(fData[i]);
			if (a > peak)
				peak = a;
		}
		return peak;
	}

	// Resamples the whole document. Bumping the stored rate instead would be
	// cheaper, but then the edit would not survive export -- the file would
	// simply play at a different rate elsewhere. Stretching the samples keeps
	// the result self-contained.
	bool ChangeSpeed(double factor)
	{
		BAutolock lock(fLock);
		if (fData == NULL || fFrames < 2 || factor <= 0)
			return false;

		int64 newFrames = (int64)(fFrames / factor);
		if (newFrames < 2)
			return false;

		float* out = (float*)malloc(newFrames * fChannels * sizeof(float));
		if (out == NULL)
			return false;

		for (int64 i = 0; i < newFrames; i++) {
			double source = i * factor;
			int64 index = (int64)source;
			double frac = source - index;
			if (index >= fFrames - 1) {
				index = fFrames - 2;
				frac = 1.0;
			}
			for (int32 c = 0; c < fChannels; c++) {
				float a = fData[index * fChannels + c];
				float b = fData[(index + 1) * fChannels + c];
				out[i * fChannels + c] = (float)(a + (b - a) * frac);
			}
		}

		free(fData);
		fData = out;
		fFrames = newFrames;
		fCapacity = newFrames;
		return true;
	}

	status_t WriteWav(const char* path) const
	{
		if (fData == NULL || fFrames == 0)
			return B_NO_INIT;
		FILE* file = fopen(path, "wb");
		if (file == NULL)
			return B_PERMISSION_DENIED;

		BAutolock lock(fLock);
		uint32 dataBytes = (uint32)(fFrames * fChannels * 2);
		uint32 u32;
		uint16 u16;
		fwrite("RIFF", 1, 4, file);
		u32 = 36 + dataBytes; fwrite(&u32, 4, 1, file);
		fwrite("WAVEfmt ", 1, 8, file);
		u32 = 16; fwrite(&u32, 4, 1, file);
		u16 = 1; fwrite(&u16, 2, 1, file);
		u16 = (uint16)fChannels; fwrite(&u16, 2, 1, file);
		u32 = fRate; fwrite(&u32, 4, 1, file);
		u32 = fRate * fChannels * 2; fwrite(&u32, 4, 1, file);
		u16 = (uint16)(fChannels * 2); fwrite(&u16, 2, 1, file);
		u16 = 16; fwrite(&u16, 2, 1, file);
		fwrite("data", 1, 4, file);
		u32 = dataBytes; fwrite(&u32, 4, 1, file);

		for (int64 i = 0; i < fFrames * fChannels; i++) {
			float v = fData[i];
			if (v > 1.0f) v = 1.0f;
			if (v < -1.0f) v = -1.0f;
			int16 s = (int16)(v * 32767.0f);
			fwrite(&s, 2, 1, file);
		}
		fclose(file);
		return B_OK;
	}

private:
	mutable	BLocker			fLock;
			float*			fData;
			int64			fFrames;
			int64			fCapacity;
			int32			fChannels;
			uint32			fRate;
};


class WaveView : public BView {
public:
								WaveView(BRect frame, SampleRing* ring,
									AudioDoc* doc);

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 code,
									const BMessage* drag);
	virtual	void				MouseUp(BPoint where);

			void				SetLive(bool live) { fLive = live; }
			void				SetPlayHead(int64 frame) { fPlayHead = frame; }

			int64				SelectionStart() const { return fSelStart; }
			int64				SelectionEnd() const { return fSelEnd; }
			bool				HasSelection() const
									{ return fSelEnd > fSelStart; }
			void				SelectAll();
			void				SelectNone();
			void				ClampSelection();

private:
			int64				_FrameAt(float x) const;
			float				_XForFrame(int64 frame) const;

			SampleRing*			fRing;
			AudioDoc*			fDoc;
			bool				fLive;
			bool				fTracking;
			int64				fAnchor;
			int64				fSelStart;
			int64				fSelEnd;
			int64				fPlayHead;
};


WaveView::WaveView(BRect frame, SampleRing* ring, AudioDoc* doc)
	:
	BView(frame, "wave", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
	fRing(ring),
	fDoc(doc),
	fLive(false),
	fTracking(false),
	fAnchor(0),
	fSelStart(0),
	fSelEnd(0),
	fPlayHead(-1)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


int64
WaveView::_FrameAt(float x) const
{
	BRect bounds = Bounds();
	float width = bounds.Width();
	if (width <= 0 || fDoc->Frames() == 0)
		return 0;
	double ratio = (x - bounds.left) / width;
	if (ratio < 0)
		ratio = 0;
	if (ratio > 1)
		ratio = 1;
	return (int64)(ratio * fDoc->Frames());
}


float
WaveView::_XForFrame(int64 frame) const
{
	BRect bounds = Bounds();
	if (fDoc->Frames() == 0)
		return bounds.left;
	return bounds.left + bounds.Width() * ((double)frame / fDoc->Frames());
}


void
WaveView::SelectAll()
{
	fSelStart = 0;
	fSelEnd = fDoc->Frames();
}


void
WaveView::SelectNone()
{
	fSelStart = fSelEnd = 0;
}


void
WaveView::ClampSelection()
{
	int64 frames = fDoc->Frames();
	if (fSelStart > frames)
		fSelStart = frames;
	if (fSelEnd > frames)
		fSelEnd = frames;
	if (fSelEnd < fSelStart)
		fSelEnd = fSelStart;
}


void
WaveView::MouseDown(BPoint where)
{
	if (fLive || fDoc->Frames() == 0)
		return;
	fTracking = true;
	fAnchor = _FrameAt(where.x);
	fSelStart = fSelEnd = fAnchor;
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}


void
WaveView::MouseMoved(BPoint where, uint32, const BMessage*)
{
	if (!fTracking)
		return;
	int64 frame = _FrameAt(where.x);
	fSelStart = frame < fAnchor ? frame : fAnchor;
	fSelEnd = frame < fAnchor ? fAnchor : frame;
}


void
WaveView::MouseUp(BPoint)
{
	fTracking = false;
}


void
WaveView::Draw(BRect)
{
	BRect bounds = Bounds();
	float middle = bounds.top + bounds.Height() / 2;
	float half = bounds.Height() / 2 - 2;
	int32 columns = (int32)bounds.Width();
	if (columns <= 1)
		return;

	SetHighColor(20, 20, 24);
	FillRect(bounds);

	// selection band, behind the trace
	if (!fLive && fSelEnd > fSelStart) {
		SetHighColor(45, 60, 95);
		FillRect(BRect(_XForFrame(fSelStart), bounds.top,
			_XForFrame(fSelEnd), bounds.bottom));
	}

	SetHighColor(60, 60, 70);
	StrokeLine(BPoint(bounds.left, middle), BPoint(bounds.right, middle));

	if (fLive) {
		const int32 kPerColumn = 24;
		float* window = (float*)malloc(columns * kPerColumn * sizeof(float));
		if (window == NULL)
			return;
		fRing->ReadRecent(window, columns * kPerColumn);

		SetHighColor(120, 220, 140);
		for (int32 x = 0; x < columns; x++) {
			float lo = 1.0f, hi = -1.0f;
			for (int32 i = 0; i < kPerColumn; i++) {
				float v = window[x * kPerColumn + i];
				if (v < lo) lo = v;
				if (v > hi) hi = v;
			}
			if (lo <= hi) {
				StrokeLine(BPoint(bounds.left + x, middle - hi * half),
					BPoint(bounds.left + x, middle - lo * half));
			}
		}
		free(window);
		return;
	}

	// Held for the whole scan: the pointer and the frame count have to stay
	// consistent with each other, and with the memory they describe.
	BAutolock docLock(fDoc->Locker());

	int64 frames = fDoc->Frames();
	if (frames == 0) {
		SetHighColor(140, 140, 150);
		DrawString("no recording", BPoint(bounds.left + 12, middle));
		return;
	}

	const float* data = fDoc->Data();
	int32 channels = fDoc->Channels();
	SetHighColor(120, 220, 140);

	for (int32 x = 0; x < columns; x++) {
		int64 from = frames * x / columns;
		int64 to = frames * (x + 1) / columns;
		if (to <= from)
			to = from + 1;
		if (to > frames)
			to = frames;

		float lo = 1.0f, hi = -1.0f;
		for (int64 f = from; f < to; f++) {
			for (int32 c = 0; c < channels; c++) {
				float v = data[f * channels + c];
				if (v < lo) lo = v;
				if (v > hi) hi = v;
			}
		}
		if (lo <= hi) {
			StrokeLine(BPoint(bounds.left + x, middle - hi * half),
				BPoint(bounds.left + x, middle - lo * half));
		}
	}

	if (fPlayHead >= 0 && fPlayHead <= frames) {
		float x = _XForFrame(fPlayHead);

		// A bare hairline is hard to follow on a small screen, so shade the
		// part already played and put a wider marker at the edge of it.
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(255, 210, 100, 40);
		float from = fSelEnd > fSelStart ? _XForFrame(fSelStart) : bounds.left;
		if (x > from)
			FillRect(BRect(from, bounds.top, x, bounds.bottom));
		SetDrawingMode(B_OP_COPY);

		SetHighColor(255, 215, 100);
		StrokeLine(BPoint(x, bounds.top), BPoint(x, bounds.bottom));
		StrokeLine(BPoint(x - 1, bounds.top), BPoint(x - 1, bounds.bottom));
	}
}


class RecorderWindow : public BWindow {
public:
								RecorderWindow();
	virtual						~RecorderWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	void				DispatchMessage(BMessage* message,
									BHandler* target);
	virtual	bool				QuitRequested();

			void				PushSamples(const void* data, size_t size,
									const media_format& format);
			void				FillPlayback(void* buffer, size_t size,
									const media_raw_audio_format& format);

private:
			status_t			_StartRecording();
			void				_StopRecording();
			void				_DecideLayout();
			void				_AppendFrame(float left, float right);

			status_t			_StartPlayback();
			void				_StopPlayback();
			void				_UpdateTransport();
			void				_ApplyLanguage(bool korean);
			void				_Save(const entry_ref& directory,
									const char* name);

			void				_UpdateStatus();
			void				_SelectionRange(int64* from, int64* to);

			SampleRing			fRing;
			AudioDoc			fDoc;
			WaveView*			fWave;
			BMenu*				fFileMenu;
			BMenu*				fEditMenu;
			BMenu*				fVolumeMenu;
			BMenu*				fSpeedMenu;
			BMenu*				fLanguageMenu;
			BMenuItem*			fSaveItem;
			BMenuItem*			fQuitItem;
			BMenuItem*			fSelectAllItem;
			BMenuItem*			fSelectNoneItem;
			BMenuItem*			fDeleteItem;
			BMenuItem*			fTrimItem;
			BMenuItem*			fSilenceItem;
			BMenuItem*			fLouderItem;
			BMenuItem*			fQuieterItem;
			BMenuItem*			fNormalizeItem;
			BMenuItem*			fFasterItem;
			BMenuItem*			fSlowerItem;
			BMenuItem*			fEnglishItem;
			BMenuItem*			fKoreanItem;
			BMenuItem*			fAutoItem;
			BMenuItem*			fMonoItem;
			BMenuItem*			fStereoItem;
			BFilePanel*			fSavePanel;
			// Forces _UpdateTransport() to push labels once after a language
			// change, even though the play/stop state has not moved.
			bool				fRelabel;
			BButton*			fRecordButton;
			BButton*			fPlayButton;
			BMenuField*			fModeField;
			BStringView*		fStatus;
			BMessageRunner*		fRunner;

			BBitmap*			fRecordIcon;
			BBitmap*			fPlayIcon;
			BBitmap*			fStopIcon;

			BMediaRecorder*		fRecorder;
			media_node			fInput;
			bool				fHaveInput;
			bool				fRecording;

			BSoundPlayer*		fPlayer;
	volatile bool				fPlaying;
	volatile int64				fPlayFrame;
			int64				fPlayLimit;
	volatile int32				fCallbackCount;
			int32				fTick;
	// Peak of what we actually hand the mixer, so "no sound" can be told
	// apart from "we are writing silence".
	volatile float				fPlayPeak;
	// Peak alone cannot tell a real take from silence with one click in it,
	// so the average level goes out next to it.
	volatile double				fPlaySumSquares;
	volatile int64				fPlaySampleCount;
			int32				fPlayChannels;
			bool				fShowingStopIcon;

			uint32				fRate;
			double				fPeak;
			double				fSumSquares;
			int64				fSampleCount;

			int32				fRequestedMode;
			int32				fOutputChannels;
			bool				fLayoutDecided;
			float*				fPending;
			int32				fPendingFrames;
			int32				fPendingCapacity;
			double				fDiffSum;
			double				fMagSum;
};


static void
record_hook(void* cookie, bigtime_t, void* data, size_t size,
	const media_format& format)
{
	((RecorderWindow*)cookie)->PushSamples(data, size, format);
}


static void
play_hook(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	((RecorderWindow*)cookie)->FillPlayback(buffer, size, format);
}


RecorderWindow::RecorderWindow()
	:
	BWindow(BRect(100, 100, 800, 470), "R Sound Editor", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
	fRunner(NULL),
	fRecordIcon(NULL),
	fPlayIcon(NULL),
	fStopIcon(NULL),
	fRecorder(NULL),
	fHaveInput(false),
	fRecording(false),
	fPlayer(NULL),
	fPlaying(false),
	fPlayFrame(0),
	fPlayLimit(0),
	fCallbackCount(0),
	fTick(0),
	fPlayPeak(0),
	fPlaySumSquares(0),
	fPlaySampleCount(0),
	fPlayChannels(0),
	fShowingStopIcon(false),
	fSavePanel(NULL),
	fRelabel(false),
	fRate(48000),
	fPeak(0),
	fSumSquares(0),
	fSampleCount(0),
	fRequestedMode(kChannelAuto),
	fOutputChannels(1),
	fLayoutDecided(false),
	fPending(NULL),
	fPendingFrames(0),
	fPendingCapacity(0),
	fDiffSum(0),
	fMagSum(0)
{
	BRect bounds = Bounds();

	BMenuBar* menuBar = new BMenuBar(BRect(0, 0, bounds.right, 18), "menu");

	// Every menu and item is kept so _ApplyLanguage() can relabel them in
	// place; rebuilding the bar instead would drop the shortcuts and the
	// currently marked language.
	fFileMenu = new BMenu(T(kStrFile));
	fFileMenu->AddItem(fSaveItem
		= new BMenuItem(T(kStrSave), new BMessage(kMsgSave), 'S'));
	fFileMenu->AddSeparatorItem();
	fFileMenu->AddItem(fQuitItem
		= new BMenuItem(T(kStrQuit), new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(fFileMenu);

	fEditMenu = new BMenu(T(kStrEdit));
	fEditMenu->AddItem(fSelectAllItem
		= new BMenuItem(T(kStrSelectAll), new BMessage(kMsgSelectAll), 'A'));
	fEditMenu->AddItem(fSelectNoneItem
		= new BMenuItem(T(kStrSelectNone), new BMessage(kMsgSelectNone)));
	fEditMenu->AddSeparatorItem();
	fEditMenu->AddItem(fDeleteItem
		= new BMenuItem(T(kStrDelete), new BMessage(kMsgCut), 'X'));
	fEditMenu->AddItem(fTrimItem
		= new BMenuItem(T(kStrTrim), new BMessage(kMsgTrim), 'T'));
	fEditMenu->AddItem(fSilenceItem
		= new BMenuItem(T(kStrSilence), new BMessage(kMsgSilence)));
	menuBar->AddItem(fEditMenu);

	fVolumeMenu = new BMenu(T(kStrVolume));
	fVolumeMenu->AddItem(fLouderItem
		= new BMenuItem(T(kStrLouder), new BMessage(kMsgLouder), '+'));
	fVolumeMenu->AddItem(fQuieterItem
		= new BMenuItem(T(kStrQuieter), new BMessage(kMsgQuieter), '-'));
	fVolumeMenu->AddItem(fNormalizeItem
		= new BMenuItem(T(kStrNormalize), new BMessage(kMsgNormalize), 'N'));
	menuBar->AddItem(fVolumeMenu);

	fSpeedMenu = new BMenu(T(kStrSpeed));
	fSpeedMenu->AddItem(fFasterItem
		= new BMenuItem(T(kStrFaster), new BMessage(kMsgFaster)));
	fSpeedMenu->AddItem(fSlowerItem
		= new BMenuItem(T(kStrSlower), new BMessage(kMsgSlower)));
	menuBar->AddItem(fSpeedMenu);

	fLanguageMenu = new BMenu(T(kStrLanguage));
	fLanguageMenu->SetRadioMode(true);
	BMessage* englishMessage = new BMessage(kMsgLanguage);
	englishMessage->AddBool("korean", false);
	fEnglishItem = new BMenuItem(T(kStrEnglish), englishMessage);
	BMessage* koreanMessage = new BMessage(kMsgLanguage);
	koreanMessage->AddBool("korean", true);
	fKoreanItem = new BMenuItem(T(kStrKorean), koreanMessage);
	fLanguageMenu->AddItem(fEnglishItem);
	fLanguageMenu->AddItem(fKoreanItem);
	(sStrings == kStringsKo ? fKoreanItem : fEnglishItem)->SetMarked(true);
	menuBar->AddItem(fLanguageMenu);

	AddChild(menuBar);

	BRect waveFrame = bounds;
	waveFrame.top = menuBar->Bounds().bottom + 1;
	waveFrame.bottom -= 44;
	fWave = new WaveView(waveFrame, &fRing, &fDoc);
	AddChild(fWave);

	// The transport strip needs a background of its own. A window's top view
	// is white and cannot be recoloured from here -- BView::Parent() returns
	// NULL for a window's direct children by design, so there is no handle on
	// it -- while the buttons and the channel menu field paint themselves in
	// B_PANEL_BACKGROUND_COLOR. Without this strip the menu field reads as a
	// grey patch stuck onto a white footer. Added before the controls so it
	// stays behind them.
	BRect footerFrame = bounds;
	footerFrame.top = waveFrame.bottom + 1;
	BView* footer = new BView(footerFrame, "footer",
		B_FOLLOW_LEFT_RIGHT | B_FOLLOW_BOTTOM, B_WILL_DRAW);
	footer->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	AddChild(footer);

	rgb_color red = { 220, 60, 60, 255 };
	rgb_color green = { 90, 200, 110, 255 };
	rgb_color grey = { 220, 220, 225, 255 };
	fRecordIcon = make_icon('circ', red);
	fPlayIcon = make_icon('tria', green);
	fStopIcon = make_icon('squa', grey);

	fRecordButton = new BButton(BRect(10, bounds.bottom - 38, 52,
		bounds.bottom - 8), "rec", "", new BMessage(kMsgToggleRecord),
		B_FOLLOW_LEFT | B_FOLLOW_BOTTOM);
	fRecordButton->SetIcon(fRecordIcon);
	fRecordButton->SetToolTip(T(kStrRecord));
	// Stays visibly depressed for as long as a take is running. The icon is
	// left alone here on purpose: swapping it to a stop square as well would
	// say the same thing twice and lose the "this is the record button"
	// affordance while it is held down.
	fRecordButton->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
	AddChild(fRecordButton);

	fPlayButton = new BButton(BRect(58, bounds.bottom - 38, 100,
		bounds.bottom - 8), "play", "", new BMessage(kMsgTogglePlay),
		B_FOLLOW_LEFT | B_FOLLOW_BOTTOM);
	fPlayButton->SetIcon(fPlayIcon);
	fPlayButton->SetToolTip(T(kStrPlay));
	fPlayButton->SetEnabled(false);
	AddChild(fPlayButton);

	BPopUpMenu* modeMenu = new BPopUpMenu("mode");
	fAutoItem = new BMenuItem(T(kStrAuto), new BMessage(kMsgMode));
	fMonoItem = new BMenuItem(T(kStrMono), new BMessage(kMsgMode));
	fStereoItem = new BMenuItem(T(kStrStereo), new BMessage(kMsgMode));
	modeMenu->AddItem(fAutoItem);
	modeMenu->AddItem(fMonoItem);
	modeMenu->AddItem(fStereoItem);
	fAutoItem->SetMarked(true);

	fModeField = new BMenuField(BRect(108, bounds.bottom - 38, 286,
		bounds.bottom - 8), "modefield", T(kStrChannels), modeMenu,
		B_FOLLOW_LEFT | B_FOLLOW_BOTTOM);
	fModeField->SetDivider(64);
	AddChild(fModeField);
	modeMenu->SetTargetForItems(this);

	fStatus = new BStringView(BRect(296, bounds.bottom - 32,
		bounds.right - 10, bounds.bottom - 12), "status", T(kStrReady),
		B_FOLLOW_LEFT_RIGHT | B_FOLLOW_BOTTOM);
	AddChild(fStatus);

	// The status text paints its own background; low colour as well, or the
	// glyphs keep a white box around them.
	fStatus->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fStatus->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);

	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster != NULL && roster->GetAudioInput(&fInput) == B_OK)
		fHaveInput = true;
	else
		fStatus->SetText(T(kStrNoInput));

	// BWindow does not deliver B_PULSE to its own MessageReceived -- pulses
	// go to views that asked for them -- so drive the periodic refresh with
	// an explicit runner instead. Without this the status line never updated
	// and the play button was never enabled after a take.
	// 33 ms drives the live trace; the status line only needs a third of
	// that, so it is updated every third tick.
	//
	// This used to be a thread inside WaveView that locked the window and
	// invalidated. That put the view, the sample ring and the document in
	// reach of a thread with a lifetime of its own, and the app died twice
	// with segment violations in that thread -- once alongside a second fault
	// in the window thread, which is what a shared object being torn down
	// under a reader looks like. Everything now happens on the window thread,
	// so there is nothing left to race with.
	fRunner = new BMessageRunner(BMessenger(this), new BMessage(kMsgTick),
		33000);
}


RecorderWindow::~RecorderWindow()
{
	delete fRunner;
	_StopPlayback();
	_StopRecording();
	delete fRecorder;
	delete fRecordIcon;
	delete fPlayIcon;
	delete fStopIcon;
	free(fPending);
	if (fHaveInput)
		BMediaRoster::Roster()->ReleaseNode(fInput);
}


void
RecorderWindow::_SelectionRange(int64* from, int64* to)
{
	if (fWave->HasSelection()) {
		*from = fWave->SelectionStart();
		*to = fWave->SelectionEnd();
	} else {
		*from = 0;
		*to = fDoc.Frames();
	}
}


void
RecorderWindow::PushSamples(const void* data, size_t size,
	const media_format& format)
{
	uint32 bytes = format.u.raw_audio.format
		& media_raw_audio_format::B_AUDIO_SIZE_MASK;
	if (bytes == 0)
		return;
	size_t count = size / bytes;
	uint32 inChannels = format.u.raw_audio.channel_count;
	if (inChannels == 0)
		inChannels = 1;

	for (size_t i = 0; i < count; i += inChannels) {
		float frame[2] = { 0.0f, 0.0f };

		for (uint32 c = 0; c < inChannels && c < 2; c++) {
			size_t index = i + c;
			if (index >= count)
				break;
			double v = 0;
			switch (format.u.raw_audio.format) {
				case media_raw_audio_format::B_AUDIO_FLOAT:
					v = ((const float*)data)[index]; break;
				case media_raw_audio_format::B_AUDIO_INT:
					v = ((const int32*)data)[index] / 2147483648.0; break;
				case media_raw_audio_format::B_AUDIO_SHORT:
					v = ((const int16*)data)[index] / 32768.0; break;
				case media_raw_audio_format::B_AUDIO_CHAR:
					v = ((const int8*)data)[index] / 128.0; break;
				default: break;
			}
			frame[c] = (float)v;
		}
		if (inChannels == 1)
			frame[1] = frame[0];

		// The scope shows the mix, so a mono source is not drawn at half
		// amplitude just because it arrives on two channels.
		float display = (frame[0] + frame[1]) / 2;
		fRing.Push(display);

		double a = display < 0 ? -display : display;
		if (a > fPeak)
			fPeak = a;
		fSumSquares += display * display;
		fSampleCount++;

		if (!fLayoutDecided) {
			fDiffSum += fabs(frame[0] - frame[1]);
			fMagSum += (fabs(frame[0]) + fabs(frame[1])) / 2;
			if (fPending != NULL && fPendingFrames < fPendingCapacity) {
				fPending[fPendingFrames * 2] = frame[0];
				fPending[fPendingFrames * 2 + 1] = frame[1];
				fPendingFrames++;
			}
			if (fPendingFrames >= fPendingCapacity)
				_DecideLayout();
			continue;
		}

		_AppendFrame(frame[0], frame[1]);
	}
}


void
RecorderWindow::_AppendFrame(float left, float right)
{
	if (fOutputChannels == 1) {
		float mono = (left + right) / 2;
		fDoc.Append(&mono);
	} else {
		float pair[2] = { left, right };
		fDoc.Append(pair);
	}
}


void
RecorderWindow::_DecideLayout()
{
	// Two threads reach this: the capture callback once the detection window
	// has filled, and the window thread when a take is stopped before that.
	// The bare fLayoutDecided test let both through, so both flushed fPending
	// and both freed it. Re-check under the document lock, which this is going
	// to take for Reset() and Append() anyway.
	BAutolock lock(fDoc.Locker());
	if (fLayoutDecided)
		return;

	if (fRequestedMode == kChannelMono)
		fOutputChannels = 1;
	else if (fRequestedMode == kChannelStereo)
		fOutputChannels = 2;
	else {
		// Auto. A mono microphone reaches us as the same signal on both
		// sides, so the two differ only by noise. Compare the mean difference
		// against the mean magnitude rather than an absolute threshold, so
		// the verdict does not depend on how loud the input happens to be.
		// With no signal at all, prefer mono: that is what a built-in
		// microphone almost always is.
		double ratio = fMagSum > 0 ? fDiffSum / fMagSum : 0.0;
		fOutputChannels = ratio < 0.05 ? 1 : 2;
	}

	fLayoutDecided = true;
	fDoc.Reset(fOutputChannels, fRate);

	// Flush the opening that was held back while deciding, so nothing is lost.
	for (int32 i = 0; i < fPendingFrames; i++)
		_AppendFrame(fPending[i * 2], fPending[i * 2 + 1]);

	free(fPending);
	fPending = NULL;
	fPendingFrames = 0;
	fPendingCapacity = 0;
}


status_t
RecorderWindow::_StartRecording()
{
	if (!fHaveInput)
		return B_ERROR;

	_StopPlayback();

	fRecorder = new BMediaRecorder("R Sound Editor", B_MEDIA_RAW_AUDIO);
	if (fRecorder->InitCheck() != B_OK) {
		status_t status = fRecorder->InitCheck();
		delete fRecorder;
		fRecorder = NULL;
		return status;
	}

	fRecorder->SetHooks(record_hook, NULL, this);

	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;

	status_t status = fRecorder->Connect(fInput, NULL, &format);
	if (status != B_OK) {
		delete fRecorder;
		fRecorder = NULL;
		return status;
	}

	fRate = (uint32)fRecorder->AcceptedFormat().u.raw_audio.frame_rate;
	if (fRate == 0)
		fRate = 48000;

	fLayoutDecided = false;
	fPendingFrames = 0;
	fPendingCapacity = (int32)(fRate * kAutoDetectSeconds);
	free(fPending);
	fPending = (float*)malloc(fPendingCapacity * 2 * sizeof(float));
	if (fPending == NULL)
		fPendingCapacity = 0;
	fDiffSum = 0;
	fMagSum = 0;
	if (fRequestedMode != kChannelAuto || fPendingCapacity == 0)
		_DecideLayout();

	fPeak = 0;
	fSumSquares = 0;
	fSampleCount = 0;

	// Set before Start(): the capture callback can deliver its first buffer
	// (and _DecideLayout() can free and reallocate the document) the instant
	// the recorder starts, and while fLive is false Draw reads the document.
	fWave->SelectNone();
	fWave->SetLive(true);

	status = fRecorder->Start();
	if (status != B_OK) {
		fWave->SetLive(false);
		fRecorder->Disconnect();
		delete fRecorder;
		fRecorder = NULL;
		return status;
	}

	fRecording = true;
	fWave->SelectNone();
	return B_OK;
}


void
RecorderWindow::_StopRecording()
{
	if (!fRecording)
		return;
	fRecording = false;

	// A take shorter than the detection window still has to be committed.
	if (!fLayoutDecided)
		_DecideLayout();

	if (fRecorder != NULL) {
		fRecorder->Stop();
		fRecorder->Disconnect();
		delete fRecorder;
		fRecorder = NULL;
	}

	fWave->SetLive(false);
	fWave->SelectNone();
}


status_t
RecorderWindow::_StartPlayback()
{
	if (fDoc.Frames() == 0)
		return B_NO_INIT;

	int64 from, to;
	_SelectionRange(&from, &to);
	fPlayFrame = from;
	fPlayLimit = to;

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.frame_rate = (float)fDoc.Rate();
	format.channel_count = fDoc.Channels();
	format.format = media_raw_audio_format::B_AUDIO_SHORT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = 4096;

	fPlayer = new BSoundPlayer(&format, "R Sound Editor", play_hook, NULL, this);
	if (fPlayer->InitCheck() != B_OK) {
		status_t status = fPlayer->InitCheck();
		delete fPlayer;
		fPlayer = NULL;
		return status;
	}

	fCallbackCount = 0;
	fPlayPeak = 0;
	fPlaySumSquares = 0;
	fPlaySampleCount = 0;
	fPlayChannels = fPlayer->Format().channel_count;
	fPlaying = true;

	// The mixer hands every new player a volume of its own, and it does not
	// have to be the one the last player had. Left alone it has been observed
	// at zero here, which plays a perfectly correct buffer to nobody.
	fPlayer->SetVolume(1.0f);
	fPlayer->SetHasData(true);
	fPlayer->Start();
	return B_OK;
}


void
RecorderWindow::_StopPlayback()
{
	fPlaying = false;
	if (fPlayer != NULL) {
		fPlayer->SetHasData(false);
		fPlayer->Stop();
		delete fPlayer;
		fPlayer = NULL;
	}
	fWave->SetPlayHead(-1);
	fWave->Invalidate();
}


void
RecorderWindow::FillPlayback(void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	BAutolock docLock(fDoc.Locker());

	int16* out = (int16*)buffer;
	size_t samples = size / sizeof(int16);
	const float* data = fDoc.Data();
	int32 docChannels = fDoc.Channels();

	// Use the channel count the player actually negotiated, not the
	// document's: the mixer is free to hand back something else, and reading
	// the buffer with the wrong stride writes interleaved noise (or nothing
	// audible at all) no matter how correct the samples are.
	int32 outChannels = format.channel_count;
	if (outChannels < 1)
		outChannels = 1;
	if (docChannels < 1)
		docChannels = 1;

	fCallbackCount++;
	float peak = 0;
	double sumSquares = 0;
	int64 counted = 0;

	for (size_t i = 0; i + outChannels <= samples; i += outChannels) {
		if (data == NULL || !fPlaying || fPlayFrame >= fPlayLimit) {
			for (int32 c = 0; c < outChannels; c++)
				out[i + c] = 0;
			continue;
		}
		for (int32 c = 0; c < outChannels; c++) {
			// Duplicate a mono document across however many output channels
			// the player wants, and drop extra document channels if it wants
			// fewer.
			int32 sourceChannel = c < docChannels ? c : docChannels - 1;
			float v = data[fPlayFrame * docChannels + sourceChannel];
			if (v > 1.0f) v = 1.0f;
			if (v < -1.0f) v = -1.0f;
			float mag = v < 0 ? -v : v;
			if (mag > peak)
				peak = mag;
			sumSquares += (double)v * v;
			counted++;
			out[i + c] = (int16)(v * 32767.0f);
		}
		fPlayFrame++;
	}

	if (peak > fPlayPeak)
		fPlayPeak = peak;
	fPlaySumSquares = fPlaySumSquares + sumSquares;
	fPlaySampleCount = fPlaySampleCount + counted;

	// Tearing the player down here would mean joining this very thread, so
	// hand that back to the window.
	if (fPlaying && fPlayFrame >= fPlayLimit) {
		fPlaying = false;
		PostMessage(kMsgPlaybackFinished);
	}
}


void
RecorderWindow::_UpdateStatus()
{
	char text[320];
	if (fRecording) {
		double rms = fSampleCount > 0 ? sqrt(fSumSquares / fSampleCount) : 0;
		snprintf(text, sizeof(text),
			T(kStrRecordingFmt), fDoc.Duration(), fRate,
			!fLayoutDecided ? T(kStrDetecting)
				: (fOutputChannels == 1 ? T(kStrMono) : T(kStrStereo)),
			fPeak > 0 ? 20 * log10(fPeak) : -99.0,
			rms > 0 ? 20 * log10(rms) : -99.0);
	} else if (fPlayer != NULL) {
		double at = (double)fPlayFrame / fDoc.Rate();
		double end = (double)fPlayLimit / fDoc.Rate();
		float peak = fPlayPeak;
		int64 counted = fPlaySampleCount;
		double rms = counted > 0 ? sqrt(fPlaySumSquares / counted) : 0;
		snprintf(text, sizeof(text),
			T(kStrPlayingFmt), at, end, fPlayChannels, fCallbackCount,
			peak > 0 ? 20 * log10(peak) : -99.0,
			rms > 0 ? 20 * log10(rms) : -99.0,
			fPlayer->Volume());
	} else if (fDoc.Frames() > 0) {
		int64 from, to;
		_SelectionRange(&from, &to);
		if (fWave->HasSelection()) {
			snprintf(text, sizeof(text),
				T(kStrSelectionFmt),
				fDoc.Duration(),
				fDoc.Channels() == 1 ? T(kStrMono) : T(kStrStereo),
				(double)from / fDoc.Rate(), (double)to / fDoc.Rate(),
				(double)(to - from) / fDoc.Rate());
		} else {
			snprintf(text, sizeof(text),
				T(kStrHintFmt), fDoc.Duration(),
				fDoc.Channels() == 1 ? T(kStrMono) : T(kStrStereo));
		}
	} else {
		snprintf(text, sizeof(text), "%s", T(kStrReady));
	}
	fStatus->SetText(text);
	_UpdateTransport();
}


// Delete and Backspace remove the selected region, matching the Edit menu's
// "Delete selection". Handled here rather than through AddShortcut(), which
// insists on a modifier; the keys have to work on their own. Anything that is
// not one of those two, or that arrives with no selection to act on, falls
// through untouched so the menu shortcuts still work.
void
RecorderWindow::DispatchMessage(BMessage* message, BHandler* target)
{
	if (message->what == B_KEY_DOWN) {
		int8 byte;
		if (message->FindInt8("byte", &byte) == B_OK
			&& (byte == B_DELETE || byte == B_BACKSPACE)
			&& !fRecording && fWave->HasSelection()) {
			PostMessage(kMsgCut);
			return;
		}
	}

	BWindow::DispatchMessage(message, target);
}


void
RecorderWindow::_UpdateTransport()
{
	// Both buttons are derived from the actual state in one place. They used
	// to be poked at each call site, so every new path that stopped playback
	// was another chance to leave a stop square sitting on an idle button.
	//
	// This also runs on every 100 ms tick, so the icon swap is guarded:
	// SetIcon() re-renders the button, and doing that ten times a second for
	// no reason is a visible flicker and real work on this CPU.
	fRecordButton->SetValue(fRecording ? B_CONTROL_ON : B_CONTROL_OFF);
	fRecordButton->SetToolTip(T(fRecording ? kStrStopRecording : kStrRecord));

	bool playing = fPlayer != NULL;
	if (playing != fShowingStopIcon || fRelabel) {
		fShowingStopIcon = playing;
		fPlayButton->SetIcon(playing ? fStopIcon : fPlayIcon);
		fPlayButton->SetToolTip(T(playing ? kStrStop : kStrPlay));
	}
	fPlayButton->SetEnabled(playing || (fDoc.Frames() > 0 && !fRecording));
}


void
RecorderWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgToggleRecord:
		{
			if (!fRecording) {
				status_t status = _StartRecording();
				if (status != B_OK) {
					BString text(T(kStrCannotRecord));
					text << strerror(status);
					BAlert* alert = new BAlert("R Sound Editor", text.String(),
						"OK");
					alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
					alert->Go(NULL);
					_UpdateTransport();
					break;
				}
			} else {
				_StopRecording();
			}
			_UpdateTransport();
			break;
		}

		case kMsgTogglePlay:
		{
			if (fPlayer != NULL) {
				_StopPlayback();
			} else if (!fRecording) {
				status_t status = _StartPlayback();
				if (status != B_OK) {
					BString text(T(kStrCannotPlay));
					text << strerror(status);
					BAlert* alert = new BAlert("R Sound Editor", text.String(),
						"OK");
					alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
					alert->Go(NULL);
					break;
				}
			}
			_UpdateTransport();
			break;
		}

		case kMsgPlaybackFinished:
			_StopPlayback();
			_UpdateTransport();
			break;

		case kMsgMode:
		{
			// Changing this mid-take would corrupt the interleaving, so it
			// only affects the next recording.
			if (fModeField != NULL && fModeField->Menu() != NULL) {
				BMenuItem* marked = fModeField->Menu()->FindMarked();
				if (marked != NULL)
					fRequestedMode = fModeField->Menu()->IndexOf(marked);
			}
			break;
		}

		case kMsgSelectAll:
			fWave->SelectAll();
			break;

		case kMsgSelectNone:
			fWave->SelectNone();
			break;

		case kMsgCut:
			if (!fRecording && fWave->HasSelection()) {
				_StopPlayback();
				_UpdateTransport();
				fDoc.Remove(fWave->SelectionStart(), fWave->SelectionEnd());
				fWave->SelectNone();
			}
			break;

		case kMsgTrim:
			if (!fRecording && fWave->HasSelection()) {
				_StopPlayback();
				_UpdateTransport();
				fDoc.KeepOnly(fWave->SelectionStart(), fWave->SelectionEnd());
				fWave->SelectNone();
			}
			break;

		case kMsgSilence:
			if (!fRecording && fWave->HasSelection()) {
				fDoc.Scale(fWave->SelectionStart(), fWave->SelectionEnd(),
					0.0f);
			}
			break;

		case kMsgLouder:
		case kMsgQuieter:
		{
			if (fRecording || fDoc.Frames() == 0)
				break;
			int64 from, to;
			_SelectionRange(&from, &to);
			// +/-3 dB: the smallest step that is clearly audible.
			fDoc.Scale(from, to,
				message->what == kMsgLouder ? 1.4125f : 0.7079f);
			break;
		}

		case kMsgNormalize:
		{
			if (fRecording || fDoc.Frames() == 0)
				break;
			int64 from, to;
			_SelectionRange(&from, &to);
			float peak = fDoc.PeakIn(from, to);
			// Leave a little headroom rather than pinning to full scale.
			if (peak > 0.0001f)
				fDoc.Scale(from, to, 0.95f / peak);
			break;
		}

		case kMsgFaster:
		case kMsgSlower:
		{
			if (fRecording || fDoc.Frames() == 0)
				break;
			_StopPlayback();
			_UpdateTransport();
			// Resampling moves pitch along with speed: this is the plain
			// "run the tape faster", not time-stretching.
			fDoc.ChangeSpeed(message->what == kMsgFaster ? 1.25 : 0.8);
			fWave->SelectNone();
			break;
		}

		case kMsgSave:
		{
			if (fDoc.Frames() == 0)
				break;

			// Kept across invocations so the panel reopens where it was left,
			// which is what every other Haiku app does.
			if (fSavePanel == NULL) {
				fSavePanel = new BFilePanel(B_SAVE_PANEL,
					new BMessenger(this), NULL, 0, false);
			}
			fSavePanel->Window()->SetTitle(T(kStrSavePanel));
			fSavePanel->SetSaveText(T(kStrDefaultName));
			fSavePanel->Show();
			break;
		}

		case B_SAVE_REQUESTED:
		{
			entry_ref directory;
			const char* name = NULL;
			if (message->FindRef("directory", &directory) == B_OK
				&& message->FindString("name", &name) == B_OK) {
				_Save(directory, name);
			}
			break;
		}

		case kMsgLanguage:
		{
			bool korean = false;
			message->FindBool("korean", &korean);
			_ApplyLanguage(korean);
			break;
		}

		case kMsgTick:
		{
			// The playhead lives in the view but is driven by the playback
			// thread's frame counter, so it has to be pushed in on the tick.
			// Nothing else invalidates the view while playing, which is why
			// setting it only on teardown left it permanently invisible.
			if (fPlayer != NULL)
				fWave->SetPlayHead(fPlayFrame);

			if (fRecording || fPlayer != NULL)
				fWave->Invalidate();

			if (++fTick >= 3) {
				fTick = 0;
				fWave->ClampSelection();
				_UpdateStatus();
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
RecorderWindow::_Save(const entry_ref& directory, const char* name)
{
	BPath path(&directory);
	if (path.InitCheck() != B_OK) {
		if (find_directory(B_USER_DIRECTORY, &path) != B_OK)
			path.SetTo("/boot/home");
	}
	path.Append(name);

	status_t status = fDoc.WriteWav(path.Path());
	BString text;
	if (status == B_OK)
		text << T(kStrSaved) << path.Path();
	else
		text << T(kStrCannotSave) << strerror(status);

	BAlert* alert = new BAlert("R Sound Editor", text.String(), "OK");
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(NULL);
}


void
RecorderWindow::_ApplyLanguage(bool korean)
{
	sStrings = korean ? kStringsKo : kStringsEn;

	fFileMenu->Superitem()->SetLabel(T(kStrFile));
	fSaveItem->SetLabel(T(kStrSave));
	fQuitItem->SetLabel(T(kStrQuit));

	fEditMenu->Superitem()->SetLabel(T(kStrEdit));
	fSelectAllItem->SetLabel(T(kStrSelectAll));
	fSelectNoneItem->SetLabel(T(kStrSelectNone));
	fDeleteItem->SetLabel(T(kStrDelete));
	fTrimItem->SetLabel(T(kStrTrim));
	fSilenceItem->SetLabel(T(kStrSilence));

	fVolumeMenu->Superitem()->SetLabel(T(kStrVolume));
	fLouderItem->SetLabel(T(kStrLouder));
	fQuieterItem->SetLabel(T(kStrQuieter));
	fNormalizeItem->SetLabel(T(kStrNormalize));

	fSpeedMenu->Superitem()->SetLabel(T(kStrSpeed));
	fFasterItem->SetLabel(T(kStrFaster));
	fSlowerItem->SetLabel(T(kStrSlower));

	fLanguageMenu->Superitem()->SetLabel(T(kStrLanguage));
	fEnglishItem->SetLabel(T(kStrEnglish));
	fKoreanItem->SetLabel(T(kStrKorean));
	(korean ? fKoreanItem : fEnglishItem)->SetMarked(true);

	fAutoItem->SetLabel(T(kStrAuto));
	fMonoItem->SetLabel(T(kStrMono));
	fStereoItem->SetLabel(T(kStrStereo));
	fModeField->SetLabel(T(kStrChannels));
	// SetLabel() on the item alone is not enough: the field draws from its own
	// superitem, which copied the label when the item was marked. Copy it over
	// or the popup keeps showing the previous language.
	if (BMenuItem* marked = fModeField->Menu()->FindMarked())
		fModeField->MenuItem()->SetLabel(marked->Label());

	if (!fHaveInput)
		fStatus->SetText(T(kStrNoInput));

	// The transport labels are only pushed when the play/stop state changes,
	// so nudge it explicitly or the tooltips keep the old language.
	fRelabel = true;
	_UpdateTransport();
	fRelabel = false;
	_UpdateStatus();
}


bool
RecorderWindow::QuitRequested()
{
	_StopPlayback();
	_StopRecording();
	return true;
}


static const char* const kAppSignature = "application/x-vnd.RSoundEditor";


int
main(void)
{
	// Running twice is genuinely bad here -- a second instance registers its
	// own media nodes and competes for the mixer with the window you are
	// actually looking at, and the extra windows are indistinguishable on
	// screen. That is handled by B_SINGLE_LAUNCH in the app resources, not
	// here: an IsRunning() check in this spot looks right and is not, because
	// the roster pre-registers the team under the signature *before* the
	// binary runs. Launched from Deskbar the app therefore found "itself"
	// already running and quit on the spot, while starting it from a shell --
	// where there is no pre-registration -- worked fine.

	// Start in the user's language rather than making them find the menu. The
	// locale roster is asked for the preferred list, not the single "language"
	// setting, because that is what actually reflects the Locale preferences.
	BMessage preferred;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&preferred) == B_OK) {
		const char* language = NULL;
		if (preferred.FindString("language", 0, &language) == B_OK
			&& language != NULL && strncmp(language, "ko", 2) == 0) {
			sStrings = kStringsKo;
		}
	}

	BApplication app(kAppSignature);
	RecorderWindow* window = new RecorderWindow();
	window->Show();
	app.Run();
	return 0;
}

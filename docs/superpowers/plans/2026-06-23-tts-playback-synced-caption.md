# TTS Playback Synced Caption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make assistant text reveal gradually with real TTS playback progress instead of displaying the whole sentence before speech starts.

**Architecture:** Add a small pure C++ caption progress helper that maps played audio duration to a UTF-8-safe text prefix. Add a playback-progress callback to `AudioService`, fired after each PCM chunk is written to the codec. `Application` caches the current TTS sentence and updates the display from that callback, with reset/finalize behavior on TTS start/stop/abort.

**Tech Stack:** ESP-IDF C++, FreeRTOS task callbacks, existing `Display::SetChatMessage`, host-side `g++` regression test.

---

### Task 1: Caption Progress Helper

**Files:**
- Create: `main/tts_caption_sync.h`
- Create: `main/tts_caption_sync.cc`
- Create: `tests/tts_caption_sync_test.cpp`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/tts_caption_sync_test.cpp` that constructs a `TtsCaptionSync`, sets Chinese and ASCII text, advances progress, and asserts UTF-8-safe prefixes plus monotonic display length.

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
g++ -std=c++17 -I main tests\tts_caption_sync_test.cpp main\tts_caption_sync.cc -o build\host_tests\tts_caption_sync_test.exe
```

Expected: FAIL because `main\tts_caption_sync.cc` does not exist yet.

- [ ] **Step 3: Implement minimal helper**

Create a helper with:
- `void Reset()`
- `void StartSentence(std::string text, uint32_t estimated_duration_ms)`
- `std::string Update(uint32_t played_ms)`
- `std::string Finish()`

It must avoid splitting UTF-8 continuation bytes and keep output length monotonic.

- [ ] **Step 4: Run host test to verify it passes**

Run the same compile command, then:

```powershell
build\host_tests\tts_caption_sync_test.exe
```

Expected: exit code 0.

### Task 2: Audio Playback Progress Callback

**Files:**
- Modify: `main/audio/audio_service.h`
- Modify: `main/audio/audio_service.cc`

- [ ] **Step 1: Extend callbacks**

Add `std::function<void(uint32_t)> on_playback_progress;` to `AudioServiceCallbacks`.

- [ ] **Step 2: Emit progress after actual PCM output**

In `AudioOutputTask`, accumulate played milliseconds from `task->pcm.size()` and `codec_->output_sample_rate()` after `codec_->OutputData(task->pcm)`, then call `callbacks_.on_playback_progress(played_ms)`.

- [ ] **Step 3: Reset progress with decoder reset**

Clear the accumulated played duration in `ResetDecoder()` so each TTS turn starts at zero.

### Task 3: Application Display Integration

**Files:**
- Modify: `main/application.h`
- Modify: `main/application.cc`

- [ ] **Step 1: Add TTS caption state**

Add mutex-protected caption state and helper methods:
- `ResetTtsCaption()`
- `StartTtsCaption(const std::string& text)`
- `UpdateTtsCaption(uint32_t played_ms)`
- `FinishTtsCaption()`

- [ ] **Step 2: Wire playback callback**

In `Application::Start`, set `callbacks.on_playback_progress` to call `UpdateTtsCaption`.

- [ ] **Step 3: Replace immediate assistant display**

On `tts.sentence`, call `StartTtsCaption(text)` instead of immediately setting the full assistant message.

- [ ] **Step 4: Reset and finalize lifecycle**

Reset on `tts.start`, abort, sleep, channel close, and decoder resets. On `tts.stop`, call `FinishTtsCaption()` so the final text is visible once speech completes or the stop event arrives.

### Task 4: Verification

**Files:**
- No new files.

- [ ] **Step 1: Run host regression test**

Run:

```powershell
g++ -std=c++17 -I main tests\tts_caption_sync_test.cpp main\tts_caption_sync.cc -o build\host_tests\tts_caption_sync_test.exe
build\host_tests\tts_caption_sync_test.exe
```

Expected: exit code 0.

- [ ] **Step 2: Run ESP-IDF build**

Run the existing ESP-IDF build command for this project.

Expected: exit code 0 and `xiaozhi.bin` generated.

/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#define COBJMACROS

#include "interface/audiodev.h"

#include "capture_queue.h"
#include "format.h"

#include "common/debug.h"
#include "common/thread.h"
#include "common/windebug.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WASAPI_CAPTURE_QUEUE_PACKETS 8
#define WASAPI_QPC_TICK_NS           INT64_C(100)
#define WASAPI_SENDER_WAIT_MS        100

static const GUID wasapi_pcmSubFormat   =
{
  UINT32_C(0x00000001), UINT16_C(0x0000), UINT16_C(0x0010),
  { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

static const GUID wasapi_floatSubFormat =
{
  UINT32_C(0x00000003), UINT16_C(0x0000), UINT16_C(0x0010),
  { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

typedef enum
{
  WASAPI_PLAYBACK_COMMAND_NONE,
  WASAPI_PLAYBACK_COMMAND_START,
  WASAPI_PLAYBACK_COMMAND_STOP,
  WASAPI_PLAYBACK_COMMAND_CONTROL,
  WASAPI_PLAYBACK_COMMAND_EXIT
}
WasapiPlaybackCommand;

typedef enum
{
  WASAPI_RECORD_COMMAND_NONE,
  WASAPI_RECORD_COMMAND_CONTROL,
  WASAPI_RECORD_COMMAND_EXIT
}
WasapiRecordCommand;

typedef struct
{
  CRITICAL_SECTION commandLock;
  bool             commandLockInitialized;
  LGThread       * thread;
  HANDLE           commandEvent;
  HANDLE           audioEvent;
  HANDLE           commandDone;
  volatile LONG    command;
  bool             commandResult;
  _Atomic(bool)    terminated;

  LG_AudioFormat format;
  int            stride;
  int            maxPeriodFrames;
  int            startFrames;
  LG_AudioPullFn pullFn;

  LG_AudioFailureFn failureFn;
  uint32_t          failureCookie;
  bool              active;

  IAudioClient        * client;
  IAudioRenderClient  * render;
  IChannelAudioVolume * channelVolume;
  ISimpleAudioVolume  * simpleVolume;
  UINT32                bufferFrames;
  REFERENCE_TIME        streamLatency;
  _Atomic(uint64_t)     latencyUs;

  UINT32               controlChannels;
  float                controlVolume[LG_AUDIO_MAX_CHANNELS];
  bool                 controlMute;
  WasapiControlPending controlPending;
}
WasapiPlayback;

typedef struct
{
  CRITICAL_SECTION commandLock;
  bool             commandLockInitialized;
  LGThread       * thread;
  HANDLE           commandEvent;
  HANDLE           audioEvent;
  HANDLE           commandDone;
  volatile LONG    command;
  bool             commandResult;
  _Atomic(bool)    terminated;

  LG_AudioFormat    format;
  int               stride;
  LG_AudioPushFn    pushFn;
  LG_AudioFailureFn failureFn;
  uint32_t          failureCookie;
  bool              active;

  IAudioClient        * client;
  IAudioCaptureClient * capture;
  IChannelAudioVolume * channelVolume;
  ISimpleAudioVolume  * simpleVolume;
  UINT32                bufferFrames;
  uint8_t             * silence;
  bool                  discontinuityPending;
  WasapiCaptureQueue    queue;
  LGThread            * senderThread;
  HANDLE                senderEvent;
  _Atomic(bool)         senderStop;
  _Atomic(bool)         senderFailed;

  UINT32               controlChannels;
  float                controlVolume[LG_AUDIO_MAX_CHANNELS];
  bool                 controlMute;
  WasapiControlPending controlPending;
}
WasapiRecord;

struct Wasapi
{
  WasapiPlayback playback;
  WasapiRecord   record;
};

static struct Wasapi wasapi = { 0 };

static bool wasapi_wait(HANDLE handle, const char * operation)
{
  const DWORD result = WaitForSingleObject(handle, INFINITE);
  if (result == WAIT_OBJECT_0)
    return true;

  const DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
  DEBUG_WINERROR(operation, HRESULT_FROM_WIN32(error));
  return false;
}

static float wasapi_volumeLevel(uint16_t volume)
{
  double level = 9.3234e-7 * pow(1.000211902, volume) - 0.000172787;
  if (level < 0.0)
    level = 0.0;
  if (level > 1.0)
    level = 1.0;
  return (float)level;
}

static bool wasapi_waveFormat(const LG_AudioFormat * format,
    WAVEFORMATEXTENSIBLE * wave)
{
  WasapiFormatInfo info;
  if (!wasapi_formatInfo(format, &info))
    return false;

  memset(wave, 0, sizeof(*wave));
  wave->Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
  wave->Format.nChannels            = format->channelCount;
  wave->Format.nSamplesPerSec       = format->sampleRate;
  wave->Format.nAvgBytesPerSec      = info.bytesPerSecond;
  wave->Format.nBlockAlign          = info.blockAlign;
  wave->Format.wBitsPerSample       = info.bitsPerSample;
  wave->Format.cbSize               = sizeof(*wave) - sizeof(wave->Format);
  wave->Samples.wValidBitsPerSample = wave->Format.wBitsPerSample;
  wave->dwChannelMask               = info.channelMask;
  wave->SubFormat                   = info.sampleType == WASAPI_SAMPLE_TYPE_PCM ?
    wasapi_pcmSubFormat : wasapi_floatSubFormat;

  return true;
}

static HRESULT wasapi_activate(EDataFlow flow, IAudioClient ** client)
{
  IMMDeviceEnumerator * enumerator = NULL;
  IMMDevice           * device     = NULL;
  HRESULT               status     = CoCreateInstance(
      &CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
      &IID_IMMDeviceEnumerator, (void **)&enumerator);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the WASAPI device enumerator", status);
    return status;
  }

  status = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
      enumerator, flow, eConsole, &device);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get the default WASAPI endpoint", status);
    IMMDeviceEnumerator_Release(enumerator);
    return status;
  }

  status = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL,
      NULL, (void **)client);
  if (FAILED(status))
    DEBUG_WINERROR("Failed to activate the WASAPI endpoint", status);

  IMMDevice_Release(device);
  IMMDeviceEnumerator_Release(enumerator);
  return status;
}

static HANDLE wasapi_enableRealtime(void)
{
  DWORD taskIndex = 0;
  return AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
}

static void wasapi_disableRealtime(HANDLE task)
{
  if (task && !AvRevertMmThreadCharacteristics(task))
    DEBUG_WINERROR("Failed to leave the WASAPI MMCSS task",
        HRESULT_FROM_WIN32(GetLastError()));
}

static void wasapi_playbackRelease(void)
{
  if (wasapi.playback.simpleVolume)
  {
    ISimpleAudioVolume_Release(wasapi.playback.simpleVolume);
    wasapi.playback.simpleVolume    = NULL;
  }
  if (wasapi.playback.channelVolume)
  {
    IChannelAudioVolume_Release(wasapi.playback.channelVolume);
    wasapi.playback.channelVolume   = NULL;
  }
  if (wasapi.playback.render)
  {
    IAudioRenderClient_Release(wasapi.playback.render);
    wasapi.playback.render          = NULL;
  }
  if (wasapi.playback.client)
  {
    IAudioClient_Release(wasapi.playback.client);
    wasapi.playback.client          = NULL;
  }
}

static bool wasapi_playbackInitialize(void)
{
  WAVEFORMATEXTENSIBLE wave;
  if (!wasapi_waveFormat(&wasapi.playback.format, &wave))
    return false;

  HRESULT status = wasapi_activate(eRender, &wasapi.playback.client);
  if (FAILED(status))
    return false;

  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
    AUDCLNT_STREAMFLAGS_NOPERSIST |
    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

  status = IAudioClient_Initialize(wasapi.playback.client,
      AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &wave.Format, NULL);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to initialize WASAPI playback", status);
    return false;
  }

  status = IAudioClient_SetEventHandle(
      wasapi.playback.client, wasapi.playback.audioEvent);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the WASAPI playback event", status);
    return false;
  }

  status = IAudioClient_GetBufferSize(
      wasapi.playback.client, &wasapi.playback.bufferFrames);
  if (FAILED(status) || !wasapi.playback.bufferFrames ||
      wasapi.playback.bufferFrames > INT_MAX)
  {
    if (FAILED(status))
      DEBUG_WINERROR("Failed to get the WASAPI playback buffer size", status);
    return false;
  }

  status = IAudioClient_GetService(wasapi.playback.client,
      &IID_IAudioRenderClient, (void **)&wasapi.playback.render);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get the WASAPI render service", status);
    return false;
  }

  status = IAudioClient_GetStreamLatency(
      wasapi.playback.client, &wasapi.playback.streamLatency);
  if (FAILED(status))
    wasapi.playback.streamLatency = 0;

  IAudioClient_GetService(wasapi.playback.client,
      &IID_IChannelAudioVolume, (void **)&wasapi.playback.channelVolume);
  IAudioClient_GetService(wasapi.playback.client,
      &IID_ISimpleAudioVolume, (void **)&wasapi.playback.simpleVolume);

  wasapi.playback.maxPeriodFrames = wasapi.playback.bufferFrames;
  wasapi.playback.startFrames     = wasapi.playback.bufferFrames;
  return true;
}

static void wasapi_playbackUpdateLatency(UINT32 queuedFrames)
{
  const uint64_t queuedDuration = wasapi_framesToDuration(
      queuedFrames, wasapi.playback.format.sampleRate);
  const uint64_t streamLatency  = wasapi.playback.streamLatency > 0 ?
    (uint64_t)wasapi.playback.streamLatency : 0;
  atomic_store_explicit(&wasapi.playback.latencyUs,
      (queuedDuration + streamLatency) / 10, memory_order_release);
}

static bool wasapi_playbackWrite(void)
{
  UINT32  padding = 0;
  HRESULT status  = IAudioClient_GetCurrentPadding(
      wasapi.playback.client, &padding);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get WASAPI playback padding", status);
    return false;
  }

  const UINT32 available = wasapi_availableFrames(
      wasapi.playback.bufferFrames, padding);
  if (!available)
  {
    wasapi_playbackUpdateLatency(padding);
    return true;
  }

  BYTE * data = NULL;
  status = IAudioRenderClient_GetBuffer(
      wasapi.playback.render, available, &data);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to acquire the WASAPI playback buffer", status);
    return false;
  }

  int frames = wasapi.playback.pullFn(data, (int)available);
  if (frames < 0 || frames > (int)available)
  {
    DEBUG_WARN("WASAPI playback returned an invalid frame count: %d", frames);
    frames = frames < 0 ? 0 : (int)available;
  }

  if (frames < (int)available)
  {
    memset(data + (size_t)frames * wasapi.playback.stride,
        wasapi_silenceByte(wasapi.playback.format.sampleFormat),
        (size_t)(available - frames) * wasapi.playback.stride);
  }

  status = IAudioRenderClient_ReleaseBuffer(
      wasapi.playback.render, available, 0);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to submit the WASAPI playback buffer", status);
    return false;
  }

  wasapi_playbackUpdateLatency(padding + available);
  return true;
}

static void wasapi_playbackStopWorker(void)
{
  if (wasapi.playback.active)
  {
    const HRESULT status = IAudioClient_Stop(wasapi.playback.client);
    if (FAILED(status))
      DEBUG_WINERROR("Failed to stop WASAPI playback", status);
    wasapi.playback.active = false;
  }

  if (wasapi.playback.client)
  {
    const HRESULT status = IAudioClient_Reset(wasapi.playback.client);
    if (FAILED(status) && status != AUDCLNT_E_NOT_STOPPED)
      DEBUG_WINERROR("Failed to reset WASAPI playback", status);
  }

  wasapi.playback.failureFn     = NULL;
  wasapi.playback.failureCookie = 0;
  atomic_store_explicit(
      &wasapi.playback.latencyUs, 0, memory_order_release);
}

static bool wasapi_playbackStartWorker(void)
{
  if (wasapi.playback.active)
    return true;

  HRESULT status = IAudioClient_Reset(wasapi.playback.client);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to reset WASAPI playback", status);
    return false;
  }

  if (!wasapi_playbackWrite())
    return false;

  status = IAudioClient_Start(wasapi.playback.client);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to start WASAPI playback", status);
    return false;
  }

  wasapi.playback.active = true;
  return true;
}

static void wasapi_playbackApplyControl(void)
{
  const WasapiControlPending pending =
    wasapi_controlPendingTake(&wasapi.playback.controlPending);
  if (pending.volume &&
      wasapi.playback.channelVolume)
  {
    const HRESULT status = IChannelAudioVolume_SetAllVolumes(
        wasapi.playback.channelVolume,
        wasapi.playback.controlChannels,
        wasapi.playback.controlVolume, NULL);
    if (FAILED(status))
      DEBUG_WINERROR("Failed to set WASAPI playback volume", status);
  }

  if (pending.mute &&
      wasapi.playback.simpleVolume)
  {
    const HRESULT status = ISimpleAudioVolume_SetMute(
        wasapi.playback.simpleVolume,
        wasapi.playback.controlMute, NULL);
    if (FAILED(status))
      DEBUG_WINERROR("Failed to set WASAPI playback mute", status);
  }
}

static void wasapi_playbackFail(void)
{
  if (!wasapi.playback.active)
    return;

  IAudioClient_Stop(wasapi.playback.client);
  wasapi.playback.active = false;
  atomic_store_explicit(
      &wasapi.playback.latencyUs, 0, memory_order_release);

  LG_AudioFailureFn failureFn     = wasapi.playback.failureFn;
  const uint32_t    failureCookie = wasapi.playback.failureCookie;
  wasapi.playback.failureFn       = NULL;
  wasapi.playback.failureCookie   = 0;
  if (failureFn)
    failureFn(failureCookie);
}

static int wasapi_playbackThread(void * opaque)
{
  (void)opaque;

  const HRESULT comStatus      = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  const bool    comInitialized = SUCCEEDED(comStatus);
  HANDLE        realtime       = NULL;
  if (!comInitialized)
  {
    DEBUG_WINERROR("Failed to initialize COM for WASAPI playback", comStatus);
    wasapi.playback.commandResult = false;
    atomic_store_explicit(
        &wasapi.playback.terminated, true, memory_order_release);
    SetEvent(wasapi.playback.commandDone);
    return 0;
  }

  realtime                      = wasapi_enableRealtime();
  wasapi.playback.commandResult = wasapi_playbackInitialize();
  if (!wasapi.playback.commandResult)
    atomic_store_explicit(
        &wasapi.playback.terminated, true, memory_order_release);
  SetEvent(wasapi.playback.commandDone);
  if (!wasapi.playback.commandResult)
    goto exit;

  const HANDLE events[] =
  {
    wasapi.playback.commandEvent,
    wasapi.playback.audioEvent
  };

  for (;;)
  {
    const DWORD result = WaitForMultipleObjects(
        sizeof(events) / sizeof(*events), events, false, INFINITE);
    if (result == WAIT_OBJECT_0)
    {
      const WasapiPlaybackCommand command =
        (WasapiPlaybackCommand)InterlockedExchange(
            &wasapi.playback.command, WASAPI_PLAYBACK_COMMAND_NONE);
      switch (command)
      {
        case WASAPI_PLAYBACK_COMMAND_START:
          wasapi.playback.commandResult = wasapi_playbackStartWorker();
          if (!wasapi.playback.commandResult)
          {
            wasapi.playback.failureFn     = NULL;
            wasapi.playback.failureCookie = 0;
          }
          SetEvent(wasapi.playback.commandDone);
          break;

        case WASAPI_PLAYBACK_COMMAND_STOP:
          wasapi_playbackStopWorker();
          wasapi.playback.commandResult = true;
          SetEvent(wasapi.playback.commandDone);
          break;

        case WASAPI_PLAYBACK_COMMAND_CONTROL:
          wasapi_playbackApplyControl();
          wasapi.playback.commandResult = true;
          SetEvent(wasapi.playback.commandDone);
          break;

        case WASAPI_PLAYBACK_COMMAND_EXIT:
          wasapi_playbackStopWorker();
          wasapi.playback.commandResult = true;
          goto exitCommand;

        case WASAPI_PLAYBACK_COMMAND_NONE:
          wasapi.playback.commandResult = false;
          SetEvent(wasapi.playback.commandDone);
          break;
      }
    }
    else if (result == WAIT_OBJECT_0 + 1)
    {
      if (wasapi.playback.active && !wasapi_playbackWrite())
        wasapi_playbackFail();
    }
    else
    {
      const DWORD error = result == WAIT_FAILED ?
        GetLastError() : ERROR_GEN_FAILURE;
      DEBUG_WINERROR("Failed to wait for WASAPI playback",
          HRESULT_FROM_WIN32(error));
      wasapi_playbackFail();
      wasapi.playback.commandResult = false;
      goto exitCommand;
    }
  }

exitCommand:
  wasapi_playbackRelease();
  wasapi_disableRealtime(realtime);
  CoUninitialize();
  atomic_store_explicit(
      &wasapi.playback.terminated, true, memory_order_release);
  SetEvent(wasapi.playback.commandDone);
  return 0;

exit:
  wasapi_playbackRelease();
  wasapi_disableRealtime(realtime);
  CoUninitialize();
  return 0;
}

static bool wasapi_playbackSendLocked(WasapiPlaybackCommand command)
{
  if (atomic_load_explicit(
        &wasapi.playback.terminated, memory_order_acquire))
    return false;

  InterlockedExchange(&wasapi.playback.command, command);
  wasapi.playback.commandResult = false;
  const bool signaled = SetEvent(wasapi.playback.commandEvent) != 0;
  const bool waited   = signaled && wasapi_wait(
      wasapi.playback.commandDone, "Failed to wait for WASAPI playback");
  return waited && wasapi.playback.commandResult;
}

static bool wasapi_playbackSend(WasapiPlaybackCommand command)
{
  EnterCriticalSection(&wasapi.playback.commandLock);
  const bool result = wasapi_playbackSendLocked(command);
  LeaveCriticalSection(&wasapi.playback.commandLock);
  return result;
}

static void wasapi_playbackClose(void)
{
  EnterCriticalSection(&wasapi.playback.commandLock);
  if (wasapi.playback.thread)
  {
    wasapi_playbackSendLocked(WASAPI_PLAYBACK_COMMAND_EXIT);
    lgJoinThread(wasapi.playback.thread, NULL);
    wasapi.playback.thread = NULL;
  }

  if (wasapi.playback.commandDone)
    CloseHandle(wasapi.playback.commandDone);
  if (wasapi.playback.audioEvent)
    CloseHandle(wasapi.playback.audioEvent);
  if (wasapi.playback.commandEvent)
    CloseHandle(wasapi.playback.commandEvent);
  wasapi.playback.commandDone  = NULL;
  wasapi.playback.audioEvent   = NULL;
  wasapi.playback.commandEvent = NULL;
  wasapi.playback.pullFn       = NULL;
  LeaveCriticalSection(&wasapi.playback.commandLock);
}

static bool wasapi_playbackSetup(const LG_AudioFormat * format,
    int requestedPeriodFrames, bool requestResampler,
    bool * resamplerEnabled, int * maxPeriodFrames, int * startFrames,
    LG_AudioPullFn pullFn)
{
  wasapi_playbackClose();
  *resamplerEnabled = false;
  (void)requestResampler;

  WAVEFORMATEXTENSIBLE wave;
  if (!format || requestedPeriodFrames <= 0 || !pullFn ||
      !wasapi_waveFormat(format, &wave))
    return false;

  wasapi.playback.format          = *format;
  wasapi.playback.stride          = wave.Format.nBlockAlign;
  wasapi.playback.maxPeriodFrames = 0;
  wasapi.playback.startFrames     = 0;
  wasapi.playback.pullFn          = pullFn;
  wasapi.playback.command         = WASAPI_PLAYBACK_COMMAND_NONE;
  atomic_store_explicit(
      &wasapi.playback.terminated, false, memory_order_relaxed);
  wasapi_controlPendingReset(&wasapi.playback.controlPending);
  atomic_store_explicit(
      &wasapi.playback.latencyUs, 0, memory_order_relaxed);

  wasapi.playback.commandEvent = CreateEventW(NULL, false, false, NULL);
  wasapi.playback.audioEvent   = CreateEventW(NULL, false, false, NULL);
  wasapi.playback.commandDone  = CreateEventW(NULL, false, false, NULL);
  if (!wasapi.playback.commandEvent || !wasapi.playback.audioEvent ||
      !wasapi.playback.commandDone)
  {
    DEBUG_WINERROR("Failed to create WASAPI playback events",
        HRESULT_FROM_WIN32(GetLastError()));
    wasapi_playbackClose();
    return false;
  }

  if (!lgCreateThread("WASAPI Playback", wasapi_playbackThread, NULL,
        &wasapi.playback.thread))
  {
    wasapi_playbackClose();
    return false;
  }

  const bool initialized = wasapi_wait(wasapi.playback.commandDone,
      "Failed to initialize WASAPI playback");
  if (!initialized || !wasapi.playback.commandResult)
  {
    if (!initialized && !atomic_load_explicit(
          &wasapi.playback.terminated, memory_order_acquire))
    {
      InterlockedExchange(&wasapi.playback.command,
          WASAPI_PLAYBACK_COMMAND_EXIT);
      if (!SetEvent(wasapi.playback.commandEvent))
        DEBUG_WINERROR("Failed to stop WASAPI playback initialization",
            HRESULT_FROM_WIN32(GetLastError()));
    }
    lgJoinThread(wasapi.playback.thread, NULL);
    wasapi.playback.thread = NULL;
    wasapi_playbackClose();
    return false;
  }

  *maxPeriodFrames  = wasapi.playback.maxPeriodFrames;
  *startFrames      = wasapi.playback.startFrames;
  return true;
}

static bool wasapi_playbackStart(
    LG_AudioFailureFn failureFn, uint32_t failureCookie)
{
  EnterCriticalSection(&wasapi.playback.commandLock);
  if (!wasapi.playback.thread)
  {
    LeaveCriticalSection(&wasapi.playback.commandLock);
    return false;
  }
  wasapi.playback.failureFn     = failureFn;
  wasapi.playback.failureCookie = failureCookie;
  const bool result =
    wasapi_playbackSendLocked(WASAPI_PLAYBACK_COMMAND_START);
  if (!result)
  {
    wasapi.playback.failureFn     = NULL;
    wasapi.playback.failureCookie = 0;
  }
  LeaveCriticalSection(&wasapi.playback.commandLock);
  return result;
}

static void wasapi_playbackStop(void)
{
  if (wasapi.playback.thread)
    wasapi_playbackSend(WASAPI_PLAYBACK_COMMAND_STOP);
}

static void wasapi_playbackVolume(int channels, const uint16_t volume[])
{
  if (!volume || channels <= 0)
    return;

  EnterCriticalSection(&wasapi.playback.commandLock);
  if (!wasapi.playback.thread ||
      channels != wasapi.playback.format.channelCount)
  {
    LeaveCriticalSection(&wasapi.playback.commandLock);
    return;
  }
  wasapi.playback.controlChannels        = channels;
  for (int i = 0; i < channels; ++i)
    wasapi.playback.controlVolume[i]     = wasapi_volumeLevel(volume[i]);
  wasapi.playback.controlPending.volume  = true;
  wasapi_playbackSendLocked(WASAPI_PLAYBACK_COMMAND_CONTROL);
  LeaveCriticalSection(&wasapi.playback.commandLock);
}

static void wasapi_playbackMute(bool mute)
{
  EnterCriticalSection(&wasapi.playback.commandLock);
  if (!wasapi.playback.thread)
  {
    LeaveCriticalSection(&wasapi.playback.commandLock);
    return;
  }

  wasapi.playback.controlMute         = mute;
  wasapi.playback.controlPending.mute = true;
  wasapi_playbackSendLocked(WASAPI_PLAYBACK_COMMAND_CONTROL);
  LeaveCriticalSection(&wasapi.playback.commandLock);
}

static uint64_t wasapi_playbackLatency(void)
{
  return atomic_load_explicit(
      &wasapi.playback.latencyUs, memory_order_acquire);
}

static int wasapi_recordSenderThread(void * opaque)
{
  (void)opaque;
  bool discontinuityPending = false;

  for (;;)
  {
    const DWORD result = WaitForSingleObject(
        wasapi.record.senderEvent, WASAPI_SENDER_WAIT_MS);
    if (atomic_load_explicit(
          &wasapi.record.senderStop, memory_order_acquire))
      return 0;

    if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT)
    {
      const DWORD error = result == WAIT_FAILED ?
        GetLastError() : ERROR_GEN_FAILURE;
      DEBUG_WINERROR("Failed to wait for the WASAPI recording sender",
          HRESULT_FROM_WIN32(error));
      atomic_store_explicit(
          &wasapi.record.senderFailed, true, memory_order_release);
      return 0;
    }

    const WasapiCapturePacket * packet;
    while ((packet = wasapi_captureQueueFront(&wasapi.record.queue)))
    {
      if (atomic_load_explicit(
            &wasapi.record.senderStop, memory_order_acquire))
        return 0;

      LG_AudioClock clock = packet->clock;
      if (packet->clockValid && discontinuityPending)
      {
        clock.stable         = false;
        clock.discontinuity  = true;
        discontinuityPending = false;
      }

      const bool accepted = wasapi.record.pushFn(packet->data,
          packet->frames, packet->clockValid ? &clock : NULL);
      wasapi_captureQueuePop(&wasapi.record.queue);
      if (!accepted)
        discontinuityPending = true;
    }
  }
}

static void wasapi_recordStopSender(void)
{
  atomic_store_explicit(
      &wasapi.record.senderStop, true, memory_order_release);
  if (wasapi.record.senderEvent)
    SetEvent(wasapi.record.senderEvent);
  if (wasapi.record.senderThread)
  {
    lgJoinThread(wasapi.record.senderThread, NULL);
    wasapi.record.senderThread = NULL;
  }
  wasapi_captureQueueFree(&wasapi.record.queue);
}

static void wasapi_recordRelease(void)
{
  wasapi_recordStopSender();

  if (wasapi.record.simpleVolume)
  {
    ISimpleAudioVolume_Release(wasapi.record.simpleVolume);
    wasapi.record.simpleVolume  = NULL;
  }
  if (wasapi.record.channelVolume)
  {
    IChannelAudioVolume_Release(wasapi.record.channelVolume);
    wasapi.record.channelVolume = NULL;
  }
  if (wasapi.record.capture)
  {
    IAudioCaptureClient_Release(wasapi.record.capture);
    wasapi.record.capture       = NULL;
  }
  if (wasapi.record.client)
  {
    IAudioClient_Release(wasapi.record.client);
    wasapi.record.client        = NULL;
  }
  free(wasapi.record.silence);
  wasapi.record.silence         = NULL;
}

static bool wasapi_recordInitialize(void)
{
  WAVEFORMATEXTENSIBLE wave;
  if (!wasapi_waveFormat(&wasapi.record.format, &wave))
    return false;

  HRESULT status = wasapi_activate(eCapture, &wasapi.record.client);
  if (FAILED(status))
    return false;

  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
    AUDCLNT_STREAMFLAGS_NOPERSIST |
    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  status = IAudioClient_Initialize(wasapi.record.client,
      AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &wave.Format, NULL);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to initialize WASAPI recording", status);
    return false;
  }

  status = IAudioClient_SetEventHandle(
      wasapi.record.client, wasapi.record.audioEvent);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the WASAPI recording event", status);
    return false;
  }

  status = IAudioClient_GetBufferSize(
      wasapi.record.client, &wasapi.record.bufferFrames);
  if (FAILED(status) || !wasapi.record.bufferFrames ||
      wasapi.record.bufferFrames > INT_MAX)
  {
    if (FAILED(status))
      DEBUG_WINERROR("Failed to get the WASAPI recording buffer size", status);
    return false;
  }

  if ((size_t)wasapi.record.bufferFrames >
      SIZE_MAX / (size_t)wasapi.record.stride)
    return false;

  const size_t silenceSize = (size_t)wasapi.record.bufferFrames *
    (size_t)wasapi.record.stride;
  wasapi.record.silence = malloc(silenceSize);
  if (!wasapi.record.silence)
    return false;
  memset(wasapi.record.silence,
      wasapi_silenceByte(wasapi.record.format.sampleFormat), silenceSize);

  if (!wasapi_captureQueueInit(&wasapi.record.queue,
        WASAPI_CAPTURE_QUEUE_PACKETS, silenceSize))
    return false;

  atomic_store_explicit(
      &wasapi.record.senderStop, false, memory_order_relaxed);
  atomic_store_explicit(
      &wasapi.record.senderFailed, false, memory_order_relaxed);
  if (!lgCreateThread("WASAPI Recording Sender", wasapi_recordSenderThread,
        NULL, &wasapi.record.senderThread))
    return false;

  status = IAudioClient_GetService(wasapi.record.client,
      &IID_IAudioCaptureClient, (void **)&wasapi.record.capture);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get the WASAPI capture service", status);
    return false;
  }

  IAudioClient_GetService(wasapi.record.client,
      &IID_IChannelAudioVolume, (void **)&wasapi.record.channelVolume);
  IAudioClient_GetService(wasapi.record.client,
      &IID_ISimpleAudioVolume, (void **)&wasapi.record.simpleVolume);

  status = IAudioClient_Start(wasapi.record.client);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to start WASAPI recording", status);
    return false;
  }

  wasapi.record.active               = true;
  wasapi.record.discontinuityPending = true;
  return true;
}

static void wasapi_recordApplyControl(void)
{
  const WasapiControlPending pending =
    wasapi_controlPendingTake(&wasapi.record.controlPending);
  if (pending.volume && wasapi.record.channelVolume)
  {
    const HRESULT status = IChannelAudioVolume_SetAllVolumes(
        wasapi.record.channelVolume, wasapi.record.controlChannels,
        wasapi.record.controlVolume, NULL);
    if (FAILED(status))
      DEBUG_WINERROR("Failed to set WASAPI recording volume", status);
  }

  if (pending.mute && wasapi.record.simpleVolume)
  {
    const HRESULT status = ISimpleAudioVolume_SetMute(
        wasapi.record.simpleVolume, wasapi.record.controlMute, NULL);
    if (FAILED(status))
      DEBUG_WINERROR("Failed to set WASAPI recording mute", status);
  }
}

static void wasapi_recordFail(void)
{
  if (!wasapi.record.active)
    return;

  IAudioClient_Stop(wasapi.record.client);
  wasapi.record.active = false;

  LG_AudioFailureFn failureFn     = wasapi.record.failureFn;
  const uint32_t    failureCookie = wasapi.record.failureCookie;
  wasapi.record.failureFn         = NULL;
  wasapi.record.failureCookie     = 0;
  if (failureFn)
    failureFn(failureCookie);
}

static bool wasapi_recordProcess(void)
{
  for (;;)
  {
    if (atomic_load_explicit(
          &wasapi.record.senderFailed, memory_order_acquire))
      return false;

    UINT32  packetFrames = 0;
    HRESULT status       = IAudioCaptureClient_GetNextPacketSize(
        wasapi.record.capture, &packetFrames);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to query the WASAPI recording packet", status);
      return false;
    }
    if (!packetFrames)
      return true;

    BYTE   * data           = NULL;
    UINT32   frames         = 0;
    DWORD    flags          = 0;
    UINT64   devicePosition = 0;
    UINT64   qpcPosition    = 0;
    status = IAudioCaptureClient_GetBuffer(wasapi.record.capture,
        &data, &frames, &flags, &devicePosition, &qpcPosition);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to acquire the WASAPI recording buffer", status);
      return false;
    }

    if (frames > wasapi.record.bufferFrames)
    {
      IAudioCaptureClient_ReleaseBuffer(wasapi.record.capture, frames);
      DEBUG_ERROR("WASAPI returned an oversized recording packet");
      return false;
    }

    const bool    clockValid    =
      (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 &&
      qpcPosition <= INT64_MAX / WASAPI_QPC_TICK_NS;
    const bool    discontinuity = wasapi.record.discontinuityPending ||
      (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
    LG_AudioClock clock         =
    {
      .position      = devicePosition,
      .time          = qpcPosition <= INT64_MAX / WASAPI_QPC_TICK_NS ?
        (int64_t)qpcPosition * WASAPI_QPC_TICK_NS : 0,
      .rate          = 0.0,
      .stable        = !discontinuity,
      .discontinuity = discontinuity
    };
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
      data = wasapi.record.silence;
    else if (!data)
    {
      IAudioCaptureClient_ReleaseBuffer(wasapi.record.capture, frames);
      DEBUG_ERROR("WASAPI returned a recording packet without data");
      return false;
    }

    const size_t bytes       =
      (size_t)frames * (size_t)wasapi.record.stride;
    uint8_t    * destination = wasapi_captureQueueBegin(
        &wasapi.record.queue, bytes);
    if (!destination)
    {
      wasapi.record.discontinuityPending = true;
      status = IAudioCaptureClient_ReleaseBuffer(
          wasapi.record.capture, frames);
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to release the WASAPI recording buffer",
            status);
        return false;
      }
      continue;
    }

    memcpy(destination, data, bytes);
    status = IAudioCaptureClient_ReleaseBuffer(
        wasapi.record.capture, frames);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to release the WASAPI recording buffer", status);
      return false;
    }

    wasapi_captureQueueCommit(&wasapi.record.queue, bytes, (int)frames,
        clockValid ? &clock : NULL);
    wasapi.record.discontinuityPending = !clockValid;
    if (!SetEvent(wasapi.record.senderEvent))
    {
      DEBUG_WINERROR("Failed to signal the WASAPI recording sender",
          HRESULT_FROM_WIN32(GetLastError()));
      return false;
    }
  }
}

static int wasapi_recordThread(void * opaque)
{
  (void)opaque;

  const HRESULT comStatus      = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  const bool    comInitialized = SUCCEEDED(comStatus);
  HANDLE        realtime       = NULL;
  if (!comInitialized)
  {
    DEBUG_WINERROR("Failed to initialize COM for WASAPI recording", comStatus);
    wasapi.record.commandResult = false;
    atomic_store_explicit(
        &wasapi.record.terminated, true, memory_order_release);
    SetEvent(wasapi.record.commandDone);
    return 0;
  }

  realtime                    = wasapi_enableRealtime();
  wasapi.record.commandResult = wasapi_recordInitialize();
  if (!wasapi.record.commandResult)
    atomic_store_explicit(
        &wasapi.record.terminated, true, memory_order_release);
  SetEvent(wasapi.record.commandDone);
  if (!wasapi.record.commandResult)
    goto exit;

  const HANDLE events[] =
  {
    wasapi.record.commandEvent,
    wasapi.record.audioEvent
  };

  for (;;)
  {
    const DWORD result = WaitForMultipleObjects(
        sizeof(events) / sizeof(*events), events, false, INFINITE);
    if (result == WAIT_OBJECT_0)
    {
      const WasapiRecordCommand command =
        (WasapiRecordCommand)InterlockedExchange(
            &wasapi.record.command, WASAPI_RECORD_COMMAND_NONE);
      switch (command)
      {
        case WASAPI_RECORD_COMMAND_CONTROL:
          wasapi_recordApplyControl();
          wasapi.record.commandResult = true;
          SetEvent(wasapi.record.commandDone);
          break;

        case WASAPI_RECORD_COMMAND_EXIT:
          if (wasapi.record.active)
          {
            const HRESULT status = IAudioClient_Stop(wasapi.record.client);
            if (FAILED(status))
              DEBUG_WINERROR("Failed to stop WASAPI recording", status);
            wasapi.record.active = false;
          }
          wasapi.record.failureFn     = NULL;
          wasapi.record.failureCookie = 0;
          wasapi.record.commandResult = true;
          goto exitCommand;

        case WASAPI_RECORD_COMMAND_NONE:
          wasapi.record.commandResult = false;
          SetEvent(wasapi.record.commandDone);
          break;
      }
    }
    else if (result == WAIT_OBJECT_0 + 1)
    {
      if (wasapi.record.active && !wasapi_recordProcess())
        wasapi_recordFail();
    }
    else
    {
      const DWORD error = result == WAIT_FAILED ?
        GetLastError() : ERROR_GEN_FAILURE;
      DEBUG_WINERROR("Failed to wait for WASAPI recording",
          HRESULT_FROM_WIN32(error));
      wasapi_recordFail();
      wasapi.record.commandResult = false;
      goto exitCommand;
    }
  }

exitCommand:
  wasapi_disableRealtime(realtime);
  wasapi_recordRelease();
  CoUninitialize();
  atomic_store_explicit(
      &wasapi.record.terminated, true, memory_order_release);
  SetEvent(wasapi.record.commandDone);
  return 0;

exit:
  wasapi_disableRealtime(realtime);
  wasapi_recordRelease();
  CoUninitialize();
  return 0;
}

static bool wasapi_recordSendLocked(WasapiRecordCommand command)
{
  if (atomic_load_explicit(
        &wasapi.record.terminated, memory_order_acquire))
    return false;

  InterlockedExchange(&wasapi.record.command, command);
  wasapi.record.commandResult = false;
  const bool signaled = SetEvent(wasapi.record.commandEvent) != 0;
  const bool waited   = signaled && wasapi_wait(
      wasapi.record.commandDone, "Failed to wait for WASAPI recording");
  return waited && wasapi.record.commandResult;
}

static void wasapi_recordClose(void)
{
  EnterCriticalSection(&wasapi.record.commandLock);
  if (wasapi.record.thread)
  {
    wasapi_recordSendLocked(WASAPI_RECORD_COMMAND_EXIT);
    lgJoinThread(wasapi.record.thread, NULL);
    wasapi.record.thread = NULL;
  }

  if (wasapi.record.commandDone)
    CloseHandle(wasapi.record.commandDone);
  if (wasapi.record.audioEvent)
    CloseHandle(wasapi.record.audioEvent);
  if (wasapi.record.commandEvent)
    CloseHandle(wasapi.record.commandEvent);
  if (wasapi.record.senderEvent)
    CloseHandle(wasapi.record.senderEvent);
  wasapi.record.commandDone   = NULL;
  wasapi.record.audioEvent    = NULL;
  wasapi.record.commandEvent  = NULL;
  wasapi.record.senderEvent   = NULL;
  wasapi.record.pushFn        = NULL;
  wasapi.record.failureFn     = NULL;
  wasapi.record.failureCookie = 0;
  LeaveCriticalSection(&wasapi.record.commandLock);
}

static bool wasapi_recordStart(const LG_AudioFormat * format,
    LG_AudioPushFn pushFn, LG_AudioFailureFn failureFn,
    uint32_t failureCookie)
{
  wasapi_recordClose();

  WAVEFORMATEXTENSIBLE wave;
  if (!format || !pushFn || !wasapi_waveFormat(format, &wave))
    return false;

  wasapi.record.format        = *format;
  wasapi.record.stride        = wave.Format.nBlockAlign;
  wasapi.record.pushFn        = pushFn;
  wasapi.record.failureFn     = failureFn;
  wasapi.record.failureCookie = failureCookie;
  wasapi.record.command       = WASAPI_RECORD_COMMAND_NONE;
  atomic_store_explicit(
      &wasapi.record.terminated, false, memory_order_relaxed);
  wasapi_controlPendingReset(&wasapi.record.controlPending);

  wasapi.record.commandEvent = CreateEventW(NULL, false, false, NULL);
  wasapi.record.audioEvent   = CreateEventW(NULL, false, false, NULL);
  wasapi.record.commandDone  = CreateEventW(NULL, false, false, NULL);
  wasapi.record.senderEvent  = CreateEventW(NULL, false, false, NULL);
  if (!wasapi.record.commandEvent || !wasapi.record.audioEvent ||
      !wasapi.record.commandDone || !wasapi.record.senderEvent)
  {
    DEBUG_WINERROR("Failed to create WASAPI recording events",
        HRESULT_FROM_WIN32(GetLastError()));
    wasapi_recordClose();
    return false;
  }

  if (!lgCreateThread("WASAPI Recording", wasapi_recordThread, NULL,
        &wasapi.record.thread))
  {
    wasapi_recordClose();
    return false;
  }

  const bool initialized = wasapi_wait(wasapi.record.commandDone,
      "Failed to initialize WASAPI recording");
  if (!initialized || !wasapi.record.commandResult)
  {
    if (!initialized && !atomic_load_explicit(
          &wasapi.record.terminated, memory_order_acquire))
    {
      InterlockedExchange(&wasapi.record.command,
          WASAPI_RECORD_COMMAND_EXIT);
      if (!SetEvent(wasapi.record.commandEvent))
        DEBUG_WINERROR("Failed to stop WASAPI recording initialization",
            HRESULT_FROM_WIN32(GetLastError()));
    }
    lgJoinThread(wasapi.record.thread, NULL);
    wasapi.record.thread = NULL;
    wasapi_recordClose();
    return false;
  }

  return true;
}

static void wasapi_recordStop(void)
{
  wasapi_recordClose();
}

static void wasapi_recordVolume(int channels, const uint16_t volume[])
{
  if (!volume || channels <= 0)
    return;

  EnterCriticalSection(&wasapi.record.commandLock);
  if (!wasapi.record.thread ||
      channels != wasapi.record.format.channelCount)
  {
    LeaveCriticalSection(&wasapi.record.commandLock);
    return;
  }
  wasapi.record.controlChannels        = channels;
  for (int i = 0; i < channels; ++i)
    wasapi.record.controlVolume[i]     = wasapi_volumeLevel(volume[i]);
  wasapi.record.controlPending.volume  = true;
  wasapi_recordSendLocked(WASAPI_RECORD_COMMAND_CONTROL);
  LeaveCriticalSection(&wasapi.record.commandLock);
}

static void wasapi_recordMute(bool mute)
{
  EnterCriticalSection(&wasapi.record.commandLock);
  if (!wasapi.record.thread)
  {
    LeaveCriticalSection(&wasapi.record.commandLock);
    return;
  }

  wasapi.record.controlMute         = mute;
  wasapi.record.controlPending.mute = true;
  wasapi_recordSendLocked(WASAPI_RECORD_COMMAND_CONTROL);
  LeaveCriticalSection(&wasapi.record.commandLock);
}

static bool wasapi_init(void)
{
  InitializeCriticalSection(&wasapi.playback.commandLock);
  wasapi.playback.commandLockInitialized = true;
  InitializeCriticalSection(&wasapi.record.commandLock);
  wasapi.record.commandLockInitialized   = true;
  return true;
}

static void wasapi_free(void)
{
  wasapi_playbackClose();
  wasapi_recordClose();

  if (wasapi.playback.commandLockInitialized)
  {
    DeleteCriticalSection(&wasapi.playback.commandLock);
    wasapi.playback.commandLockInitialized = false;
  }
  if (wasapi.record.commandLockInitialized)
  {
    DeleteCriticalSection(&wasapi.record.commandLock);
    wasapi.record.commandLockInitialized   = false;
  }
}

struct LG_AudioDevOps LGAD_WASAPI =
{
  .name     = "WASAPI",
  .init     = wasapi_init,
  .free     = wasapi_free,
  .playback =
  {
    .setup   = wasapi_playbackSetup,
    .start   = wasapi_playbackStart,
    .stop    = wasapi_playbackStop,
    .volume  = wasapi_playbackVolume,
    .mute    = wasapi_playbackMute,
    .latency = wasapi_playbackLatency
  },
  .record   =
  {
    .start  = wasapi_recordStart,
    .stop   = wasapi_recordStop,
    .volume = wasapi_recordVolume,
    .mute   = wasapi_recordMute
  }
};

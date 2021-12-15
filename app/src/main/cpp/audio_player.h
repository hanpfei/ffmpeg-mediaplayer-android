/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef NATIVE_AUDIO_AUDIO_PLAYER_H
#define NATIVE_AUDIO_AUDIO_PLAYER_H
#include <sys/types.h>
#include "audio_common.h"
#include "buf_manager.h"
#include "debug_utils.h"

class AudioPlayer {
  // buffer queue player interfaces
  SLObjectItf outputMixObjectItf_;
  SLObjectItf playerObjectItf_;
  SLPlayItf playItf_;
  SLAndroidSimpleBufferQueueItf playBufferQueueItf_;

  SampleFormat sampleInfo_;

  ENGINE_PLAYBACK_DATA_CALLBACK data_callback_;
  void *data_ctx_;

  sample_buf buffer_;
#ifdef ENABLE_LOG
  AndroidLog *logFile_;
#endif
  std::mutex stopMutex_;

 public:
  explicit AudioPlayer(SampleFormat *sampleFormat, SLEngineItf engine);
  ~AudioPlayer();
  SLresult Start(void);
  void Stop(void);
  void ProcessSLCallback(SLAndroidSimpleBufferQueueItf bq);
  void RegisterDataCallback(ENGINE_PLAYBACK_DATA_CALLBACK cb, void *data_ctx);
};

#endif  // NATIVE_AUDIO_AUDIO_PLAYER_H

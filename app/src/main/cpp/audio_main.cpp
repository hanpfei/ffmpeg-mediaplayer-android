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
#include <jni.h>
#include <SLES/OpenSLES_Android.h>
#include <sys/types.h>
#include <cassert>
#include <cstring>
#include <mutex>

#include "audio_common.h"
#include "audio_effect.h"
#include "audio_encoded_frame_decoder.h"
#include "audio_encoded_frame_file_writer.h"
#include "audio_frame_writer.h"
#include "audio_player.h"
#include "audio_recorder.h"
#include "jni_helper.h"
#include "media_demuxer.h"
#include "media_utils.h"
#include "video_encoded_frame_decoder.h"
#include "video_encoded_frame_writer.h"
#include "video_frame_writer.h"

extern "C" {
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include "libavutil/imgutils.h"
#include <libavutil/log.h>
#include <libavutil/timestamp.h>
#include "libswscale/swscale.h"
}

struct EchoAudioEngine {
  SLmilliHertz fastPathSampleRate_;
  uint32_t fastPathFramesPerBuf_;
  uint16_t sampleChannels_;
  uint16_t bitsPerSample_;

  SLObjectItf slEngineObj_;
  SLEngineItf slEngineItf_;

  AudioRecorder *recorder_;
  AudioPlayer *player_;
  AudioQueue *freeBufQueue_;  // Owner of the queue
  AudioQueue *recBufQueue_;   // Owner of the queue

  sample_buf *bufs_;
  uint32_t bufCount_;
  uint32_t frameCount_;
  int64_t echoDelay_;
  float echoDecay_;
  AudioDelay *delayEffect_;

  std::shared_ptr<MediaDemuxer> demuxer_;
  std::shared_ptr<AudioEncodedFrameDecoder> audio_decoder_;
  std::shared_ptr<AudioFrameWriter> audio_writer_;

  std::shared_ptr<VideoEncodedFrameDecoder> video_decoder_;
  std::shared_ptr<VideoFrameWriter> video_writer_;

  sample_buf *cur_buffer_ = nullptr;
  int samples_ = 0;
  uint32_t framesPerBuf_ = 0;

  struct SwsContext *sws_ctx_ = nullptr;
  int32_t playback_frames_ = 0;
  int32_t video_rendered_frames_ = 0;

  std::atomic<float> seek_pos_ = {0.0f};
  std::atomic<bool> paused_ = {false};
  std::mutex lock_;
};
static EchoAudioEngine engine;

bool EngineService(void *ctx, uint32_t msg, void *data);
bool PlaybackDataCallback(void *ctx, uint32_t sampleRate, uint32_t framesPerBuf,
                          uint16_t channels, uint16_t pcmFormat, void *data);

JNIEXPORT void JNICALL MainActivity_createSLEngine(
    JNIEnv *env, jclass type, jint sampleRate, jint framesPerBuf,
    jlong delayInMs, jfloat decay) {
  LOGI("MainActivity_createSLEngine delayInMs %ld, sample rate %u, framesPerBuf %u, decay %f",
       static_cast<long>(delayInMs),
       sampleRate,
       static_cast<uint32_t>(framesPerBuf),
       decay);
  auto swscalever = swscale_version();
  LOGI("swscale version %u", swscalever);

  SLresult result;
  memset(&engine, 0, sizeof(engine));

  engine.fastPathSampleRate_ = static_cast<SLmilliHertz>(sampleRate) * 1000;
  engine.fastPathFramesPerBuf_ = static_cast<uint32_t>(framesPerBuf);
  engine.sampleChannels_ = AUDIO_SAMPLE_CHANNELS;
  engine.bitsPerSample_ = SL_PCMSAMPLEFORMAT_FIXED_16;

  result = slCreateEngine(&engine.slEngineObj_, 0, NULL, 0, NULL, NULL);
  SLASSERT(result);

  result =
      (*engine.slEngineObj_)->Realize(engine.slEngineObj_, SL_BOOLEAN_FALSE);
  SLASSERT(result);

  result = (*engine.slEngineObj_)
               ->GetInterface(engine.slEngineObj_, SL_IID_ENGINE,
                              &engine.slEngineItf_);
  SLASSERT(result);

  // compute the RECOMMENDED fast audio buffer size:
  //   the lower latency required
  //     *) the smaller the buffer should be (adjust it here) AND
  //     *) the less buffering should be before starting player AFTER
  //        receiving the recorder buffer
  //   Adjust the bufSize here to fit your bill [before it busts]
  uint32_t bufSize = engine.fastPathFramesPerBuf_ * engine.sampleChannels_ *
                     engine.bitsPerSample_;
  bufSize = (bufSize + 7) >> 3;  // bits --> byte
  engine.bufCount_ = BUF_COUNT;
  engine.bufs_ = allocateSampleBufs(engine.bufCount_, bufSize);
  assert(engine.bufs_);

  engine.freeBufQueue_ = new AudioQueue(engine.bufCount_);
  engine.recBufQueue_ = new AudioQueue(engine.bufCount_);
  assert(engine.freeBufQueue_ && engine.recBufQueue_);
  for (uint32_t i = 0; i < engine.bufCount_; i++) {
    engine.freeBufQueue_->push(&engine.bufs_[i]);
  }

  engine.echoDelay_ = delayInMs;
  engine.echoDecay_ = decay;
  engine.delayEffect_ = new AudioDelay(
      engine.fastPathSampleRate_, engine.sampleChannels_, engine.bitsPerSample_,
      engine.echoDelay_, engine.echoDecay_);
  assert(engine.delayEffect_);
}

JNIEXPORT jboolean JNICALL
MainActivity_configureEcho(JNIEnv *env, jclass type,
                                                       jint delayInMs,
                                                       jfloat decay) {
  engine.echoDelay_ = delayInMs;
  engine.echoDecay_ = decay;

  engine.delayEffect_->setDelayTime(delayInMs);
  engine.delayEffect_->setDecayWeight(decay);
  return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
MainActivity_createSLBufferQueueAudioPlayer(
    JNIEnv *env, jclass type) {
  SampleFormat sampleFormat;
  memset(&sampleFormat, 0, sizeof(sampleFormat));
  sampleFormat.pcmFormat_ = (uint16_t)engine.bitsPerSample_;
  sampleFormat.framesPerBuf_ = engine.fastPathFramesPerBuf_;

  // SampleFormat.representation_ = SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
  sampleFormat.channels_ = (uint16_t)engine.sampleChannels_;
  sampleFormat.sampleRate_ = engine.fastPathSampleRate_;

  engine.player_ = new AudioPlayer(&sampleFormat, engine.slEngineItf_);
  assert(engine.player_);
  if (engine.player_ == nullptr) return JNI_FALSE;

  engine.player_->RegisterDataCallback(PlaybackDataCallback, (void *)&engine);

  return JNI_TRUE;
}

JNIEXPORT void JNICALL
MainActivity_deleteSLBufferQueueAudioPlayer(
    JNIEnv *env, jclass type) {
  if (engine.player_) {
    delete engine.player_;
    engine.player_ = nullptr;
  }
}

JNIEXPORT jboolean JNICALL
MainActivity_createAudioRecorder(JNIEnv *env,
                                                             jclass type) {
  SampleFormat sampleFormat;
  memset(&sampleFormat, 0, sizeof(sampleFormat));
  sampleFormat.pcmFormat_ = static_cast<uint16_t>(engine.bitsPerSample_);

  // SampleFormat.representation_ = SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
  sampleFormat.channels_ = engine.sampleChannels_;
  sampleFormat.sampleRate_ = engine.fastPathSampleRate_;
  sampleFormat.framesPerBuf_ = engine.fastPathFramesPerBuf_;
  engine.recorder_ = new AudioRecorder(&sampleFormat, engine.slEngineItf_);
  if (!engine.recorder_) {
    return JNI_FALSE;
  }
  engine.recorder_->SetBufQueues(engine.freeBufQueue_, engine.recBufQueue_);
  engine.recorder_->RegisterCallback(EngineService, (void *)&engine);
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
MainActivity_deleteAudioRecorder(JNIEnv *env,
                                                             jclass type) {
  if (engine.recorder_) delete engine.recorder_;

  engine.recorder_ = nullptr;
}

static void startPlay(jobject surface) {
  engine.frameCount_ = 0;
  /*
   * start player: make it into waitForData state
   */
  if (SL_BOOLEAN_FALSE == engine.player_->Start()) {
    LOGE("====%s failed", __FUNCTION__);
    return;
  }
//  engine.recorder_->Start();
}


JNIEXPORT void JNICALL
MainActivity_startPlay(JNIEnv *env, jclass type, jobject surface) {
  startPlay(surface);
}

JNIEXPORT void JNICALL
MainActivity_stopPlay(JNIEnv *env, jclass type) {
  engine.recorder_->Stop();
  engine.player_->Stop();

  delete engine.recorder_;
  delete engine.player_;
  engine.recorder_ = NULL;
  engine.player_ = NULL;
}

JNIEXPORT void JNICALL MainActivity_deleteSLEngine(
    JNIEnv *env, jclass type) {
  delete engine.recBufQueue_;
  delete engine.freeBufQueue_;
  releaseSampleBufs(engine.bufs_, engine.bufCount_);
  if (engine.slEngineObj_ != NULL) {
    (*engine.slEngineObj_)->Destroy(engine.slEngineObj_);
    engine.slEngineObj_ = NULL;
    engine.slEngineItf_ = NULL;
  }

  if (engine.delayEffect_) {
    delete engine.delayEffect_;
    engine.delayEffect_ = nullptr;
  }

  if (engine.sws_ctx_) {
    sws_freeContext(engine.sws_ctx_);
    engine.sws_ctx_ = nullptr;
  }
}

uint32_t dbgEngineGetBufCount(void) {
  uint32_t count = engine.recorder_->dbgGetDevBufCount();
  count += engine.freeBufQueue_->size();
  count += engine.recBufQueue_->size();

  LOGE(
      "Buf Disrtibutions: RecDev=%d, FreeQ=%d, "
      "RecQ=%d",
      engine.recorder_->dbgGetDevBufCount(), engine.freeBufQueue_->size(),
      engine.recBufQueue_->size());
  if (count != engine.bufCount_) {
    LOGE("====Lost Bufs among the queue(supposed = %d, found = %d)", BUF_COUNT,
         count);
  }
  return count;
}

bool PlaybackDataCallback(void *ctx, uint32_t sampleRate, uint32_t framesPerBuf,
                          uint16_t channels, uint16_t pcmFormat, void *data) {
  assert(ctx == &engine);
  assert(pcmFormat == 16);

  engine.framesPerBuf_ = framesPerBuf;
  auto *engine_ptr = static_cast<EchoAudioEngine *>(ctx);
  ++engine_ptr->playback_frames_;
  if (engine_ptr->playback_frames_ % 200 == 0) {
    LOGI("PlaybackFrames=%d, RecDev=%d, FreeQ=%d, RecQ=%d", engine_ptr->playback_frames_,
         engine_ptr->recorder_->dbgGetDevBufCount(), engine_ptr->freeBufQueue_->size(),
         engine_ptr->recBufQueue_->size());
  }
  {
    std::lock_guard<std::mutex> _l(engine.lock_);
    if (engine_ptr->demuxer_ && !engine_ptr->paused_) {
      if (engine_ptr->seek_pos_ > 0.1f) {
        engine.demuxer_->seek(static_cast<float>(engine_ptr->seek_pos_));
        engine.video_decoder_->flush();
        engine.audio_decoder_->flush();

        engine_ptr->seek_pos_ = 0.0f;
      }
      int ret = 0;
      while (engine_ptr->recBufQueue_->size() < 3 && ret == 0) {
        ret = engine_ptr->demuxer_->demuxOneFrame();
      }
    }
  }

  auto *buffer = static_cast<int16_t *>(data);
  sample_buf *freeBuf = nullptr;
  if (engine_ptr->recBufQueue_->front(&freeBuf)) {
    engine_ptr->recBufQueue_->pop();
    memcpy(data, freeBuf->buf_, sizeof(int16_t) * framesPerBuf);
    freeBuf->size_ = 0;
    engine_ptr->freeBufQueue_->push(freeBuf);
  } else {
    for (int i = 0; i < framesPerBuf; ++ i) {
      buffer[i] = 0;
    }
  }

  return true;
}

/*
 * simple message passing for player/recorder to communicate with engine
 */
bool EngineService(void *ctx, uint32_t msg, void *data) {
  assert(ctx == &engine);
  switch (msg) {
    case ENGINE_SERVICE_MSG_RETRIEVE_DUMP_BUFS: {
      *(static_cast<uint32_t *>(data)) = dbgEngineGetBufCount();
      break;
    }
    case ENGINE_SERVICE_MSG_RECORDED_AUDIO_AVAILABLE: {
      LOGI("ENGINE_SERVICE_MSG_RECORDED_AUDIO_AVAILABLE");
      // adding audio delay effect
      sample_buf *buf = static_cast<sample_buf *>(data);
      assert(engine.fastPathFramesPerBuf_ ==
             buf->size_ / engine.sampleChannels_ / (engine.bitsPerSample_ / 8));
      engine.delayEffect_->process(reinterpret_cast<int16_t *>(buf->buf_),
                                   engine.fastPathFramesPerBuf_);
      break;
    }
    default:
      assert(false);
      return false;
  }

  return true;
}

JNIEXPORT jlong JNICALL
MainActivity_playerOpen(JNIEnv *env, jclass type, jstring url) {
  jsize len = env->GetStringLength(url);
  char buff[1024] = {0};
  env->GetStringUTFRegion(url,0, len, buff);
  LOGI("playerOpen Url %s, url length: %d", buff, len);

  std::lock_guard<std::mutex> _l(engine.lock_);
  if (!engine.demuxer_) {
    engine.demuxer_ = std::make_shared<MediaDemuxer>();
    engine.demuxer_->open(buff);

    engine.audio_decoder_ = std::make_shared<AudioEncodedFrameDecoder>();
    engine.audio_decoder_->initialize(engine.demuxer_->getAudioCodecParameters());
    engine.demuxer_->setAudioPacketProcessing(engine.audio_decoder_);

    engine.audio_writer_ = std::make_shared<AudioFrameWriter>();
    engine.audio_writer_->initialize("/storage/emulated/0/Music/trailer_.pcm");
    engine.audio_writer_->setDestChannelLayout(AV_CH_LAYOUT_MONO);
    engine.audio_writer_->setDestSampleFormat(AV_SAMPLE_FMT_S16);
    engine.audio_writer_->setDestSampleRate(44100);

    engine.video_decoder_ = std::make_shared<VideoEncodedFrameDecoder>();
    engine.video_decoder_->initialize(engine.demuxer_->getVideoCodecParameters());
    engine.demuxer_->setVideoPacketProcessing(engine.video_decoder_);

    engine.video_writer_ = std::make_shared<VideoFrameWriter>();
    engine.video_writer_->initialize("/storage/emulated/0/Music/trailer.yuv");
    engine.video_decoder_->setVideoFrameProcessing(engine.video_writer_);

    engine.audio_writer_->setCallback([](const int16_t *data, int sample_rate, int channels, int samples_per_channel) {
      if (!engine.cur_buffer_) {
        engine.freeBufQueue_->front(&engine.cur_buffer_);
        engine.freeBufQueue_->pop();
      }

      if (engine.samples_ + samples_per_channel < static_cast<int>(engine.framesPerBuf_)) {
        memcpy(engine.cur_buffer_->buf_ + sizeof(int16_t) * engine.samples_ * channels, data,
               sizeof(int16_t) * samples_per_channel * channels);
        engine.samples_ += samples_per_channel;
      } else {
        int remain_samples = static_cast<int>(engine.framesPerBuf_) - engine.samples_;
        memcpy(engine.cur_buffer_->buf_ + sizeof(int16_t) * engine.samples_ * channels, data,
               sizeof(int16_t) * remain_samples * channels);
        engine.recBufQueue_->push(engine.cur_buffer_);

        engine.freeBufQueue_->front(&engine.cur_buffer_);
        engine.freeBufQueue_->pop();

        engine.samples_ = samples_per_channel - remain_samples;
        memcpy(engine.cur_buffer_->buf_, data + remain_samples * channels,
               sizeof(int16_t) * engine.samples_ * channels);
      }

      return 0;
    });

    engine.audio_decoder_->setVideoFrameProcessing(engine.audio_writer_);
  }

  engine.demuxer_->demuxOneFrame();

  return 12344;
}

JNIEXPORT jlong JNICALL
MainActivity_playerOpenAt(JNIEnv *env, jclass type, jstring url, jfloat position) {
  jsize len = env->GetStringLength(url);
  char buff[1024] = {0};
  env->GetStringUTFRegion(url,0, len, buff);
  LOGI("playerOpenAt Url %s, at %f, url length: %d\n",
       buff, static_cast<float>(position), len);
  extractAudio(buff, "/sdcard/Movies/trailer.aac");
  return 12344;
}

JNIEXPORT void JNICALL
MainActivity_playerPlay(JNIEnv *env, jclass type, jlong handle) {
  LOGI("playerPlay handle: %lld\n", static_cast<long long>(handle));
}

JNIEXPORT int JNICALL
MainActivity_playerSeek(JNIEnv *env, jclass type, jlong handle, jfloat position) {
  LOGI("playerSeek handle: %lld, position: %f\n",
       static_cast<long long>(handle), static_cast<float>(position));
  engine.seek_pos_ = static_cast<float>(position);
  return 0;
}

JNIEXPORT int JNICALL
MainActivity_playerPause(JNIEnv *env, jclass type, jlong handle) {
  LOGI("playerPause handle: %lld\n", static_cast<long long>(handle));
  engine.paused_ = !engine.paused_;
  return 0;
}

JNIEXPORT int JNICALL
MainActivity_playerClose(JNIEnv *env, jclass type, jlong handle) {
  LOGI("playerClose handle: %lld\n", static_cast<long long >(handle));
  std::lock_guard<std::mutex> _l(engine.lock_);
  if (engine.demuxer_) {
    engine.demuxer_->close();
    engine.audio_decoder_->close();
    engine.audio_writer_->close();

    engine.video_decoder_->close();
    engine.video_writer_->close();

    engine.demuxer_.reset();
    engine.audio_decoder_.reset();
    engine.audio_writer_.reset();

    engine.video_writer_.reset();
    engine.video_decoder_.reset();
  }
  return 0;
}

JNIEXPORT int JNICALL
MainActivity_playerRenderVideo(JNIEnv *env, jclass type, jlong handle, jobject surface) {
  if (!engine.demuxer_) {
    LOGW("No demuxer when playing");
    return -1;
  }
  auto pCodecCtx = engine.video_decoder_->getVideoCodecContext();
  if (!pCodecCtx) {
    LOGW("No video codec context when playing");
    return -1;
  }

  if (!engine.sws_ctx_) {
    engine.sws_ctx_ = sws_getContext(pCodecCtx->width/*视频宽度*/, pCodecCtx->height/*视频高度*/,
                                     pCodecCtx->pix_fmt/*像素格式*/,
                                     pCodecCtx->width/*目标宽度*/,
                                     pCodecCtx->height/*目标高度*/, AV_PIX_FMT_RGBA/*目标格式*/,
                                     SWS_BICUBIC/*图像转换的一些算法*/, NULL, NULL, NULL);
    if (!engine.sws_ctx_) {
      LOGW("Cannot initialize the conversion context!");
      return -1;
    }
  }

  AVFrame *pFrame = engine.video_writer_->popVideoFrame();
  if (!pFrame) {
    LOGW("No video frame when playing");
    return -1;
  }

  AVFrame *pFrameRGBA = av_frame_alloc();
  if (pFrameRGBA == NULL || pFrame == NULL) {
    av_frame_unref(pFrame);
    av_frame_free(&pFrame);
    pFrame = nullptr;
    LOGE("Could not allocate video frame.");
    return -1;
  }

  bool debug = false;
  ++engine.video_rendered_frames_;
  if (engine.video_rendered_frames_ % 50 == 0) {
    debug = true;
  }

  int videoWidth = pCodecCtx->width;
  int videoHeight = pCodecCtx->height;

  int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, videoWidth, videoHeight,1);
  if (debug) {
    LOGI("Determine required buffer size and allocate buffer %d bytes", numBytes);
  }
  uint8_t *buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
  av_image_fill_arrays(pFrameRGBA->data, pFrameRGBA->linesize, buffer, AV_PIX_FMT_RGBA,
                       pCodecCtx->width, pCodecCtx->height, 1);
  if (debug) {
    LOGI("Start to transform the format of the video frame (YUV -> RGBA)");
  }
  // Transform the format of the video frame
  sws_scale(engine.sws_ctx_, (uint8_t const *const *) pFrame->data,
            pFrame->linesize, 0, pCodecCtx->height,
            pFrameRGBA->data, pFrameRGBA->linesize);

  if (debug) {
    LOGI("Transform completed successfully and start to play");
  }

  ANativeWindow *nativeWindow = ANativeWindow_fromSurface(env, surface);
  ANativeWindow_setBuffersGeometry(nativeWindow, videoWidth, videoHeight,
                                   WINDOW_FORMAT_RGBA_8888);
  if (debug) {
    LOGI("Set the buffer size (%d x %d) of native window %p, it will scale automatically",
         videoWidth, videoHeight, nativeWindow);
  }
  ANativeWindow_Buffer windowBuffer;
  ANativeWindow_lock(nativeWindow, &windowBuffer, 0);

  // Get stride
  uint8_t *dst = (uint8_t *) windowBuffer.bits;
  int dstStride = windowBuffer.stride * 4;
  uint8_t *src = pFrameRGBA->data[0];
  int srcStride = pFrameRGBA->linesize[0];

  // Copy line because the stride of window and frame is different.
  for (int h = 0; h < videoHeight; h++) {
    memcpy(dst + h * dstStride, src + h * srcStride, srcStride);
  }
  ANativeWindow_unlockAndPost(nativeWindow);
  ANativeWindow_release(nativeWindow);

  av_free(buffer);

  av_frame_unref(pFrame);
  av_frame_free(&pFrame);

  av_frame_free(&pFrameRGBA);
  if (debug) {
    LOGI("Render done.");
  }
  return engine.video_writer_->getVideoFrames();
}

// Dalvik VM type signatures
static const JNINativeMethod gMethods[] = {
        NATIVE_METHOD(MainActivity, createSLEngine, "(IIJF)V"),
        NATIVE_METHOD(MainActivity, configureEcho, "(IF)Z"),
        NATIVE_METHOD(MainActivity, createSLBufferQueueAudioPlayer, "()Z"),
        NATIVE_METHOD(MainActivity, deleteSLBufferQueueAudioPlayer, "()V"),
        NATIVE_METHOD(MainActivity, createAudioRecorder, "()Z"),
        NATIVE_METHOD(MainActivity, deleteAudioRecorder, "()V"),
        NATIVE_METHOD(MainActivity, startPlay, "(Ljava/lang/Object;)V"),
        NATIVE_METHOD(MainActivity, stopPlay, "()V"),
        NATIVE_METHOD(MainActivity, deleteSLEngine, "()V"),

        NATIVE_METHOD(MainActivity, playerOpen, "(Ljava/lang/String;)J"),
        NATIVE_METHOD(MainActivity, playerOpenAt, "(Ljava/lang/String;F)J"),
        NATIVE_METHOD(MainActivity, playerPlay, "(J)I"),
        NATIVE_METHOD(MainActivity, playerSeek, "(JF)I"),
        NATIVE_METHOD(MainActivity, playerPause, "(J)I"),
        NATIVE_METHOD(MainActivity, playerClose, "(J)I"),
        NATIVE_METHOD(MainActivity, playerRenderVideo, "(JLjava/lang/Object;)I"),
};

int jniRegisterNativeMethods(JNIEnv* env, const char *classPathName,
                             const JNINativeMethod *nativeMethods, jint nMethods) {
  jclass clazz;
  clazz = env->FindClass(classPathName);
  if (clazz == NULL) {
    LOGW("Native registration unable to find class '%s'", classPathName);
    return JNI_FALSE;
  }
  if (env->RegisterNatives(clazz, nativeMethods, nMethods) < 0) {
    LOGW("RegisterNatives failed for '%s'", classPathName);
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

// DalvikVM calls this on startup, so we can statically register all our native methods.
jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    LOGE("JavaVM::GetEnv() failed");
    abort();
  }
  jniRegisterNativeMethods(env, "com/sample/media/ffmpeg/MainActivity",
                           gMethods, NELEM(gMethods));

  return JNI_VERSION_1_6;
}
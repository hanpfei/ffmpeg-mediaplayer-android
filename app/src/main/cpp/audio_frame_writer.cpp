//
// Created by Henry Han on 2021/12/13.
//

#include "audio_frame_writer.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "log.h"

AudioFrameWriter::AudioFrameWriter() :
        out_file_(nullptr),
        outfile_resampled(nullptr),
        src_data_size_(0),
        dest_sample_rate_(0),
        dest_channel_layout_(0),
        dest_sample_format_(AV_SAMPLE_FMT_NONE),
        swr_ctx_(nullptr),
        sw_src_data_(nullptr),
        sw_dst_data_(nullptr),
        left_samples_(0) {}

AudioFrameWriter::~AudioFrameWriter() {
    close();
}

int AudioFrameWriter::initialize(const char *dstPath) {
    if (!dstPath || *dstPath == 0) {
        LOGW("Invalid destination file path.");
        return -1;
    }
    file_path_ = dstPath;

    out_file_ = fopen("/storage/emulated/0/Music/trailer_.pcm", "wbe");
    if (!out_file_) {
        LOGW("Could not open out file: %s", strerror(errno));
        return -1;
    }

    outfile_resampled = fopen("/storage/emulated/0/Music/trailer_441.pcm", "wbe");
    if (!outfile_resampled) {
        printf("Could not open out resampled file: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int AudioFrameWriter::close() {
    if (out_file_) {
        fclose(out_file_);
        out_file_ = nullptr;
    }
    if (outfile_resampled) {
        fclose(outfile_resampled);
        outfile_resampled = nullptr;
    }

    if (sw_src_data_)
        av_freep(&sw_src_data_[0]);
    av_freep(&sw_src_data_);

    if (sw_dst_data_)
        av_freep(&sw_dst_data_[0]);
    av_freep(&sw_dst_data_);

    swr_free(&swr_ctx_);
    return 0;
}

int AudioFrameWriter::setDestSampleRate(int dest_sample_rate) {
    dest_sample_rate_ = dest_sample_rate;
    return 0;
}

int AudioFrameWriter::setDestChannelLayout(int dest_channel_layout) {
    dest_channel_layout_ = dest_channel_layout;
    return 0;
}

int AudioFrameWriter::setDestSampleFormat(enum AVSampleFormat dest_sample_format) {
    dest_sample_format_ = dest_sample_format;
    return 0;
}

int AudioFrameWriter::setCallback(callback_t &&callback) {
    callback_ = std::move(callback);
    return 0;
}

void AudioFrameWriter::processAudioFrame(AVFrame *frame) {
    //获取该采样格式每个采样是多少字节
    //一个采样中可能包含多个声道，每个声道的数据大小都是data_size
    enum AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
    int data_size = av_get_bytes_per_sample(format);
    if (data_size < 0) {
        /* This should not occur, checking just for paranoia */
        LOGW("Failed to calculate data size");
        return;
    }

    int total_data_size = data_size * frame->nb_samples * frame->channels;
    if (src_data_size_ < total_data_size) {
        src_data_.reset(new uint8_t[total_data_size]);
        uint8_t *buffer = new uint8_t[total_data_size * 2];
        if (resample_buffer_data_ && left_samples_ > 0) {
            size_t left_data_size = left_samples_ * frame->channels * data_size;
            std::memcpy(buffer, resample_buffer_data_.get(), left_data_size);
        }
        resample_buffer_data_.reset(buffer);

        src_data_size_ = total_data_size;
    }

    if (format == AV_SAMPLE_FMT_FLT || format == AV_SAMPLE_FMT_FLTP) {
        interleave(reinterpret_cast<float *>(src_data_.get()), frame->channels, frame->nb_samples, frame->data);
    }
    resample(src_data_.get(), frame, outfile_resampled);
}


int AudioFrameWriter::resample(uint8_t *audio_data, AVFrame *frame, FILE *outfile_resampled) {
    if (dest_sample_rate_ == 0 || dest_channel_layout_ == 0 || dest_sample_format_ == AV_SAMPLE_FMT_NONE) {
        LOGW("Invalid destination parameters");
        return -1;
    }
    int ret = 0;

    int dst_linesize = 0;
    if (!swr_ctx_) {
        swr_ctx_ = swr_alloc();

        av_opt_set_channel_layout(swr_ctx_, "in_channel_layout", frame->channel_layout, 0);
        av_opt_set_int(swr_ctx_, "in_sample_rate", frame->sample_rate, 0);

        enum AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
        av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", format, 0);

        av_opt_set_int(swr_ctx_, "out_sample_rate", dest_sample_rate_, 0);
        av_opt_set_channel_layout(swr_ctx_, "out_channel_layout", dest_channel_layout_, 0);
        av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", dest_sample_format_, 0);

        /* initialize the resampling context */
        if ((ret = swr_init(swr_ctx_)) < 0) {
            LOGW("Failed to initialize the resampling context");
            return -1;
        }

        /* allocate source and destination samples buffers */
        int src_linesize = 0;
        int src_nb_channels = av_get_channel_layout_nb_channels(frame->channel_layout);

        int samples_per_frame = frame->sample_rate / 100;
        ret = av_samples_alloc_array_and_samples(&sw_src_data_, &src_linesize, src_nb_channels,
                                                 samples_per_frame, format, 0);
        if (ret < 0) {
            LOGW("Could not allocate source samples\n");
            return -1;
        }

        int dest_samples_per_frame = dest_sample_rate_ / 100;

        /* buffer is going to be directly written to a rawaudio file, no alignment */
        int dst_nb_channels = av_get_channel_layout_nb_channels(dest_channel_layout_);
        ret = av_samples_alloc_array_and_samples(&sw_dst_data_, &dst_linesize, dst_nb_channels,
                                                 dest_samples_per_frame, dest_sample_format_, 0);
        if (ret < 0) {
            LOGW("Could not allocate destination samples");
            return -1;
        }
    }

    int samples_per_frame = frame->sample_rate / 100;
    int dest_samples_per_frame = dest_sample_rate_ / 100;
    int dst_nb_channels = av_get_channel_layout_nb_channels(dest_channel_layout_);

    enum AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
    int data_size = av_get_bytes_per_sample(format);
    int total_data_size = data_size * frame->nb_samples * frame->channels;

    size_t left_data_size = left_samples_ * frame->channels * data_size;
    std::memcpy(resample_buffer_data_.get() + left_data_size,
                src_data_.get(), total_data_size);
    left_samples_ += frame->nb_samples;

    int sample_position = 0;
    while (sample_position <= (left_samples_ - samples_per_frame)) {
        int buf_position = sample_position * frame->channels * data_size;
        int frame_size = samples_per_frame * frame->channels * data_size;
        std::memcpy(sw_src_data_[0],
                    resample_buffer_data_.get() + buf_position, frame_size);

        /* convert to destination format */
        ret = swr_convert(swr_ctx_, sw_dst_data_, dest_samples_per_frame, const_cast<const uint8_t **>(sw_src_data_), samples_per_frame);
        if (ret < 0) {
            LOGW("Error while converting");
            return -1;
        }

        if (out_file_) {
            fwrite(resample_buffer_data_.get() + buf_position, 1, frame_size, out_file_);
        }

        int dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels,
                                                     ret, dest_sample_format_, 1);

        if (outfile_resampled) {
            fwrite(sw_dst_data_[0], 1, dst_bufsize, outfile_resampled);
        }

        if (callback_) {
            callback_(reinterpret_cast<int16_t *>(sw_dst_data_[0]), dest_sample_rate_,
                      dst_nb_channels, dest_samples_per_frame);
        }

        sample_position += samples_per_frame;
    }

    if (sample_position != left_samples_ - samples_per_frame) {
        int buf_position = sample_position * frame->channels * data_size;
        int left_size = (left_samples_ - sample_position) * frame->channels * data_size;
        std::memmove(resample_buffer_data_.get() ,
                     resample_buffer_data_.get() + buf_position, left_size);
        left_samples_ = left_samples_ - sample_position;
    }

    const char *fmt;
    if ((ret = get_format_from_sample_fmt(&fmt, dest_sample_format_)) < 0){
        LOGW("Get format failed\n");
        return -1;
    }

    return ret;
}

template<typename SampleType>
void AudioFrameWriter::interleave(SampleType *dst, int nb_channels, int nb_samples, uint8_t **data) {
    SampleType *dstp = dst;
    SampleType **srcp = reinterpret_cast<SampleType **>(data);

    /* generate sin tone with 440Hz frequency and duplicated channels */
    for (int i = 0; i < nb_samples; i++) {
        for (int j = 0; j < nb_channels; j++) {
            dstp[j] = srcp[j][i];
        }
        dstp += nb_channels;
    }
}

int AudioFrameWriter::get_format_from_sample_fmt(const char **fmt,
                                                 enum AVSampleFormat sample_fmt) {
    *fmt = nullptr;
    //采样格式与格式字符串的对应关系
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt;
        const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
            {AV_SAMPLE_FMT_U8,  "u8",    "u8"},
            {AV_SAMPLE_FMT_S16, "s16be", "s16le"},
            {AV_SAMPLE_FMT_S32, "s32be", "s32le"},
            {AV_SAMPLE_FMT_FLT, "f32be", "f32le"},
            {AV_SAMPLE_FMT_DBL, "f64be", "f64le"},
    };

    // Traversal sample_fmt_entries list
    struct sample_fmt_entry *entry = nullptr;
    for (int i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            break;
        }
    }
    int ret = 0;
    if (entry) {
        *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
//        LOGW("Sample format %s be is %s, le is %s\n",
//             av_get_sample_fmt_name(sample_fmt), entry->fmt_be, entry->fmt_le);
    } else {
        LOGW("sample format %s is not supported as output format",
             av_get_sample_fmt_name(sample_fmt));
        ret = -1;
    }

    return ret;
}
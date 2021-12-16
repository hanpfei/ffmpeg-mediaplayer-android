
#include "media_utils.h"

#include "android_debug.h"
#include <stdint.h>
extern "C" {
#include <libavutil/log.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/samplefmt.h>

#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

void extractAudio(const char *srcPath, const char *dstPath) {
    int ret;
    AVFormatContext *in_fmt_ctx = nullptr;
    int audio_index;
    AVStream *in_stream = nullptr;
    AVCodecParameters *in_codecpar = nullptr;

    AVFormatContext *out_fmt_ctx = nullptr;
    AVOutputFormat *out_fmt = nullptr;
    AVStream *out_stream = nullptr;

    AVPacket pkt;

    LOGI("Source file path: %s, dest file path: %s", srcPath, dstPath);

    //in_fmt_ctx
    ret = avformat_open_input(&in_fmt_ctx, srcPath, nullptr, nullptr);
    if (ret < 0) {
        LOGW("avformat_open_input failed：%s", av_err2str(ret));
        goto end;
    }

    //audio_index
    audio_index = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_index < 0) {
        LOGW("Find audio stream failed: %s", av_err2str(audio_index));
        goto end;
    }

    //in_stream、in_codecpar
    in_stream = in_fmt_ctx->streams[audio_index];
    in_codecpar = in_stream->codecpar;
    if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        LOGW("The Codec type is invalid!");
        goto end;
    }

    //out_fmt_ctx
    out_fmt_ctx = avformat_alloc_context();
    out_fmt = av_guess_format(NULL, dstPath, NULL);
    out_fmt_ctx->oformat = out_fmt;
    if (!out_fmt) {
        LOGW("Cloud not guess file format");
        goto end;
    }

    //out_stream
    out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    if (!out_stream) {
        LOGW("Failed to create out stream");
        goto end;
    }

    //拷贝编解码器参数
    ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
    if (ret < 0) {
        LOGW("avcodec_parameters_copy：%s", av_err2str(ret));
        goto end;
    }
    out_stream->codecpar->codec_tag = 0;


    //创建并初始化目标文件的AVIOContext
    if ((ret = avio_open(&out_fmt_ctx->pb, dstPath, AVIO_FLAG_WRITE)) < 0) {
        LOGW("avio_open %s：%s (%s)", dstPath, av_err2str(ret), strerror(errno));
        goto end;
    }

    //initialize packet
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    //写文件头
    if ((ret = avformat_write_header(out_fmt_ctx, nullptr)) < 0) {
        LOGW("avformat_write_header：%s", av_err2str(ret));
        goto end;
    }

    while (av_read_frame(in_fmt_ctx, &pkt) == 0) {
        if (pkt.stream_index == audio_index) {
            //输入流和输出流的时间基可能不同，因此要根据时间基的不同对时间戳pts进行转换
            pkt.pts = av_rescale_q(pkt.pts, in_stream->time_base, out_stream->time_base);
            pkt.dts = pkt.pts;
            //根据时间基转换duration
            pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
            pkt.pos = -1;
            pkt.stream_index = 0;

            //写入
            av_interleaved_write_frame(out_fmt_ctx, &pkt);

            //释放packet
            av_packet_unref(&pkt);
        }
    }

    //写文件尾
    av_write_trailer(out_fmt_ctx);

    //释放资源
    end:
    if (in_fmt_ctx) avformat_close_input(&in_fmt_ctx);
    if (out_fmt_ctx) {
        if (out_fmt_ctx->pb) avio_close(out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
    }
}


static void decode(AVCodecContext *avCodecContext, AVPacket *pkt, AVFrame *avFrame,
                   FILE *outfile, FILE *outfile_resampled = nullptr);
static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt);

int decodeAudio(const char *src_file_path, const char *out_file_path) {
    LOGI("Source file path: %s, dest file path: %s", src_file_path, out_file_path);
    const AVCodec *avCodec = nullptr;
    AVCodecContext *avCodecContext = nullptr;
    AVCodecParserContext *avCodecParserContext = nullptr;
    int len, ret;
    FILE *infile = nullptr, *outfile = nullptr;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data = nullptr;
    size_t data_size;
    AVPacket *pkt = nullptr;
    AVFrame *avFrame = nullptr;
    enum AVSampleFormat avSampleFormat;
    int n_channels = 0;
    const char *fmt = nullptr;

    pkt = av_packet_alloc();

    avCodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!avCodec) {
        LOGW("Codec not found");
        goto end;
    }

    avCodecParserContext = av_parser_init(avCodec->id);
    if (!avCodecParserContext) {
        LOGW("avCodecParserContext not found");
        goto end;
    }

    avCodecContext = avcodec_alloc_context3(avCodec);
    if (!avCodecContext) {
        LOGW("Could not allocate avCodecContext");
        goto end;
    }

    if (avcodec_open2(avCodecContext, avCodec, nullptr) < 0) {
        LOGW("Could not open avCodec");
        goto end;
    }

    infile = fopen(src_file_path, "rbe");
    if (!infile) {
        LOGW("Could not open in file: %s", strerror(errno));
        goto end;
    }
    outfile = fopen(out_file_path, "wbe");
    if (!outfile) {
        LOGW("Could not open out file: %s", strerror(errno));
        goto end;
    }

    /* decode until eof */
    data = inbuf;
    data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, infile);

    while (data_size > 0) {
        //如果avFrame为空，则创建一个avFrame
        if (!avFrame) {
            if (!(avFrame = av_frame_alloc())) {
                LOGW("Could not allocate video avFrame");
                goto end;
            }
        }

        //从data中解析出avPacket数据
        ret = av_parser_parse2(avCodecParserContext, avCodecContext, &pkt->data, &pkt->size,
                               data, data_size,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            LOGW("Error while parsing");
            goto end;
        }
        //后移数组指针并更新data_size
        data += ret;
        data_size -= ret;

        //解码avPacket,并存入文件
        if (pkt->size)
            decode(avCodecContext, pkt, avFrame, outfile);

        //剩余的数据不多了，将剩余数据移动到inbuf的前部
        //并从文件中再读取一次数据
        if (data_size < AUDIO_REFILL_THRESH) {
            memmove(inbuf, data, data_size);
            data = inbuf;
            len = fread(data + data_size, 1,
                        AUDIO_INBUF_SIZE - data_size, infile);
            if (len > 0)
                data_size += len;
        }
    }

    /* flush the decoder */
    pkt->data = nullptr;
    pkt->size = 0;
    decode(avCodecContext, pkt, avFrame, outfile);

    //打印输出文件的信息
    /* print output pcm infomations, because there have no metadata of pcm */
    avSampleFormat = avCodecContext->sample_fmt;
    //采样格式如果是planar的,则获得packed版的采样格式
    //因为在decode函数中，我们将数据存为文件时，采用的是packed的存法
    if (av_sample_fmt_is_planar(avSampleFormat)) {
        //获得packed版的采样格式
        avSampleFormat = av_get_packed_sample_fmt(avSampleFormat);
    }
    n_channels = avCodecContext->channels;
    if (get_format_from_sample_fmt(&fmt, avSampleFormat) < 0)
        goto end;
    LOGW("Play the output audio file with the command: "
         "ffplay -f %s -ac %d -ar %d %s",
         fmt, n_channels, avCodecContext->sample_rate,
         out_file_path);

    //释放资源
    end:
    if (outfile) fclose(outfile);
    if (infile) fclose(infile);
    if (avCodecContext) avcodec_free_context(&avCodecContext);
    if (avCodecParserContext) av_parser_close(avCodecParserContext);
    if (avFrame) av_frame_free(&avFrame);
    if (pkt) av_packet_free(&pkt);

    return 0;
}

static void fill_samples(float *dst, int nb_channels, AVFrame *avFrame)
{
    int i, j;
    float *dstp = dst;

    float **srcp = reinterpret_cast<float **>(avFrame->data);

    /* generate sin tone with 440Hz frequency and duplicated channels */
    for (i = 0; i < avFrame->nb_samples; i++) {
        for (j = 0; j < nb_channels; j++) {
            dstp[j] = srcp[j][i];
        }
        dstp += nb_channels;
    }
}

static int resample(AVFrame *avFrame, FILE *outfile_resampled) {
    int ret = 0;
    SwrContext *swr_ctx = swr_alloc();
    av_opt_set_channel_layout(swr_ctx, "in_channel_layout", avFrame->channel_layout, 0);
    av_opt_set_channel_layout(swr_ctx, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", 48000, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    /* initialize the resampling context */
    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
    }

    /* allocate source and destination samples buffers */
    uint8_t **src_data = NULL, **dst_data = NULL;
    int src_linesize, dst_linesize;
    int src_nb_samples = 1024, dst_nb_samples, max_dst_nb_samples;
    enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_FLT, dst_sample_fmt = AV_SAMPLE_FMT_S16;

    int src_nb_channels = 0, dst_nb_channels = 0;
    src_nb_channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);

    int64_t src_ch_layout = AV_CH_LAYOUT_STEREO, dst_ch_layout = AV_CH_LAYOUT_MONO;
    src_nb_channels = av_get_channel_layout_nb_channels(src_ch_layout);
    ret = av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels,
                                             src_nb_samples, src_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate source samples\n");
    }

    int src_rate = 48000, dst_rate = 44100;
    /* compute the number of converted samples: buffering is avoided
     * ensuring that the output buffer will contain at least all the
     * converted input samples */
    max_dst_nb_samples = dst_nb_samples =
            av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

    /* buffer is going to be directly written to a rawaudio file, no alignment */
    dst_nb_channels = av_get_channel_layout_nb_channels(dst_ch_layout);
    ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels,
                                             dst_nb_samples, dst_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate destination samples\n");
    }

    fill_samples((float *)src_data[0], 2, avFrame);

    /* convert to destination format */
    ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, const_cast<const uint8_t **>(src_data), src_nb_samples);
    if (ret < 0) {
        fprintf(stderr, "Error while converting\n");
    }

    int dst_bufsize;
    dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels,
                                             ret, dst_sample_fmt, 1);
    fprintf(stdout, "dst_bufsize %d\n", dst_bufsize);
    fwrite(dst_data[0], 1, dst_bufsize, outfile_resampled);

    const char *fmt;
    if ((ret = get_format_from_sample_fmt(&fmt, dst_sample_fmt)) < 0){
        fprintf(stderr, "Get format failed\n");
    }

    if (src_data)
        av_freep(&src_data[0]);
    av_freep(&src_data);

    if (dst_data)
        av_freep(&dst_data[0]);
    av_freep(&dst_data);

    swr_free(&swr_ctx);

    return ret;
}

class AVFormatContextGuard {
public:
    AVFormatContextGuard(AVFormatContext *context)
      : context_(context) {}
    ~AVFormatContextGuard() {
        if (context_) {
            avformat_close_input(&context_);
        }
    }
private:
    AVFormatContext *context_;
};

int extractAndDecodeAudio(const char *srcPath, const char *dstPath, const char *dstResampledPath) {
    int ret = -1;
    AVFormatContext *in_fmt_ctx = nullptr;
    int audio_index = -1;
    AVStream *in_stream = nullptr;
    AVCodecParameters *in_codec_params = nullptr;

    AVPacket pkt;
    LOGI("Source file path: %s, dest file path: %s", srcPath, dstPath);

    //in_fmt_ctx
    ret = avformat_open_input(&in_fmt_ctx, srcPath, nullptr, nullptr);
    if (ret < 0) {
        LOGW("avformat_open_input failed：%s", av_err2str(ret));
        return -1;
    }

    AVFormatContextGuard ctx_guard(in_fmt_ctx);

    // Find stream index of audio stream
    audio_index = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_index < 0) {
        LOGW("Find audio stream failed: %s", av_err2str(audio_index));
        return -1;
    }

    // Get audio stream and audio codec parameters
    in_stream = in_fmt_ctx->streams[audio_index];
    in_codec_params = in_stream->codecpar;
    if (in_codec_params->codec_type != AVMEDIA_TYPE_AUDIO) {
        LOGW("The Codec type is invalid!");
        return -1;
    }

    // Initialize packet
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    const AVCodec *avCodec = nullptr;
    AVCodecContext *avCodecContext = nullptr;
    AVCodecParserContext *avCodecParserContext = nullptr;
    AVFrame *avFrame = nullptr;

    FILE *outfile = nullptr;

    avCodec = avcodec_find_decoder(in_codec_params->codec_id);
    if (!avCodec) {
        LOGW("Codec not found: %d", static_cast<int>(in_codec_params->codec_id));
        return -1;
    }

    avCodecContext = avcodec_alloc_context3(avCodec);
    if (!avCodecContext) {
        LOGW("Could not allocate avCodecContext");
        return -1;
    }

    avcodec_parameters_to_context(avCodecContext, in_codec_params);

    if (avcodec_open2(avCodecContext, avCodec, nullptr) < 0) {
        LOGW("Could not open avCodec");
        return -1;
    }

    outfile = fopen(dstPath, "wbe");
    if (!outfile) {
        LOGW("Could not open out file: %s", strerror(errno));
        return -1;
    }

    FILE *outfile_resampled = fopen(dstResampledPath, "wbe");
    if (!outfile_resampled) {
        printf("Could not open out resampled file: %s", strerror(errno));
        return -1;
    }

    while (av_read_frame(in_fmt_ctx, &pkt) == 0) {
        if (pkt.stream_index == audio_index) {
            // If avFrame has not been allocated, allocate it.
            if (!avFrame) {
                if (!(avFrame = av_frame_alloc())) {
                    LOGW("Could not allocate video avFrame");
                    ret = -1;
                    break;
                }
            }
            pkt.pos = -1;
            pkt.stream_index = 0;

            if (pkt.size) {
                decode(avCodecContext, &pkt, avFrame, outfile, outfile_resampled);
            }
            //释放packet
            av_packet_unref(&pkt);
        }
    }

    /* flush the decoder */
    pkt.data = nullptr;
    pkt.size = 0;
    decode(avCodecContext, &pkt, avFrame, outfile, outfile_resampled);

    enum AVSampleFormat avSampleFormat;
    /* print output pcm infomations, because there have no metadata of pcm */
    avSampleFormat = avCodecContext->sample_fmt;
    //采样格式如果是planar的,则获得packed版的采样格式
    //因为在decode函数中，我们将数据存为文件时，采用的是packed的存法
    if (av_sample_fmt_is_planar(avSampleFormat)) {
        //获得packed版的采样格式
        avSampleFormat = av_get_packed_sample_fmt(avSampleFormat);
    }
    int n_channels = 0;
    n_channels = avCodecContext->channels;
    const char *fmt = nullptr;
    if (get_format_from_sample_fmt(&fmt, avSampleFormat) < 0) {
        return -1;
    }
    LOGW("Play the output audio file with the command: "
         "ffplay -f %s -ac %d -ar %d %s",
         fmt, n_channels, avCodecContext->sample_rate,
         dstPath);

    enum AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;
    if (get_format_from_sample_fmt(&fmt, dst_sample_fmt) < 0) {
        return -1;
    }
    LOGW("Resampling succeeded. Play the output file with the command:\n"
                    "ffplay -f %s -channel_layout %d -channels %d -ar %d %s\n",
            fmt, static_cast<int>(AV_CH_LAYOUT_MONO), 1, 44100, dstResampledPath);

    if (outfile) fclose(outfile);
    if (outfile_resampled) fclose(outfile_resampled);
    if (avCodecContext) avcodec_free_context(&avCodecContext);
    if (avCodecParserContext) av_parser_close(avCodecParserContext);
    if (avFrame) av_frame_free(&avFrame);

    return ret;
}

/**
 * 解码avPacket,并存入文件
 */
static void decode(AVCodecContext *avCodecContext, AVPacket *pkt, AVFrame *avFrame,
                   FILE *outfile, FILE *outfile_resampled) {
    int i, ch;
    int ret, data_size;

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(avCodecContext, pkt);
    if (ret < 0) {
        LOGW("Error sending a packet for decoding: %s", av_err2str(ret));
        return;
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
        //解码出frame并存入avFrame参数
        ret = avcodec_receive_frame(avCodecContext, avFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGW("Error during decoding");
            return;
        }

        //获取该采样格式每个采样是多少字节
        //一个采样中可能包含多个声道，每个声道的数据大小都是data_size
        data_size = av_get_bytes_per_sample(avCodecContext->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            LOGW("Failed to calculate data size");
            return;
        }

        //遍历avFrame中的每一个采样数据
        for (i = 0; i < avFrame->nb_samples; i++) {
            //遍历每一个声道
            for (ch = 0; ch < avCodecContext->channels; ch++) {
                //文件中数据的排列格式：采样1声道1 采样1声道2 采样2声道1 采样2声道2...
                fwrite(avFrame->data[ch] + data_size * i, 1, data_size, outfile);
            }
        }
        resample(avFrame, outfile_resampled);
    }
}

/**
 * 根据采样格式获取be le信息
 */
static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt) {
    int i;
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

    //遍历sample_fmt_entries数组
    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    LOGW("sample format %s is not supported as output format\n",
         av_get_sample_fmt_name(sample_fmt));

    return -1;
}

void extractVideo(const char *srcPath, const char *dstPath) {
    int ret;
    AVFormatContext *in_fmt_ctx = nullptr;
    int video_index;
    AVStream *in_stream = nullptr;
    AVCodecParameters *in_codecpar = nullptr;
    AVFormatContext *out_fmt_ctx = nullptr;
    AVOutputFormat *out_fmt = nullptr;
    AVStream *out_stream = nullptr;
    AVPacket pkt;

    LOGI("Source file path: %s, dest file path: %s", srcPath, dstPath);

    //in_fmt_ctx
    ret = avformat_open_input(&in_fmt_ctx, srcPath, nullptr, nullptr);
    if (ret < 0) {
        LOGI("avformat_open_input失败：%s", av_err2str(ret));
        goto end;
    }

    //video_index
    video_index = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0) {
        LOGI("Find video stream failed: %s", av_err2str(video_index));
        goto end;
    }

    //in_stream、in_codecpar
    in_stream = in_fmt_ctx->streams[video_index];
    in_codecpar = in_stream->codecpar;
    if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        LOGI("The Codec type is invalid!");
        goto end;
    }

    //out_fmt_ctx
    out_fmt_ctx = avformat_alloc_context();
    out_fmt = av_guess_format(NULL, dstPath, NULL);
    out_fmt_ctx->oformat = out_fmt;
    if (!out_fmt) {
        LOGI("Cloud not guess file format");
        goto end;
    }

    //out_stream
    out_stream = avformat_new_stream(out_fmt_ctx, NULL);
    if (!out_stream) {
        LOGI("Failed to create out stream");
        goto end;
    }

    //拷贝编解码器参数
    ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
    if (ret < 0) {
        LOGI("avcodec_parameters_copy：%s", av_err2str(ret));
        goto end;
    }
    out_stream->codecpar->codec_tag = 0;


    //创建并初始化目标文件的AVIOContext
    if ((ret = avio_open(&out_fmt_ctx->pb, dstPath, AVIO_FLAG_WRITE)) < 0) {
        LOGI("avio_open：%s", av_err2str(ret));
        goto end;
    }

    //initialize packet
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    //写文件头
    if ((ret = avformat_write_header(out_fmt_ctx, nullptr)) < 0) {
        LOGI("avformat_write_header：%s", av_err2str(ret));
        goto end;
    }

    while (av_read_frame(in_fmt_ctx, &pkt) == 0) {
        if (pkt.stream_index == video_index) {
            //输入流和输出流的时间基可能不同，因此要根据时间基的不同对时间戳pts进行转换
            pkt.pts = av_rescale_q(pkt.pts, in_stream->time_base, out_stream->time_base);
            pkt.dts = pkt.pts;
            //根据时间基转换duration
            pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
            pkt.pos = -1;
            pkt.stream_index = 0;

            //写入
            av_interleaved_write_frame(out_fmt_ctx, &pkt);

            //释放packet
            av_packet_unref(&pkt);
        }
    }

    //写文件尾
    av_write_trailer(out_fmt_ctx);

    //释放资源
    end:
    if (in_fmt_ctx) avformat_close_input(&in_fmt_ctx);
    if (out_fmt_ctx) {
        if (out_fmt_ctx->pb) avio_close(out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
    }
}

static void yuv_save(AVFrame *avFrame, FILE *ofile);

static void decode(AVCodecContext *avCodecContext, AVFrame *avFrame, AVPacket *pkt,
                   FILE *ofile);

#define INBUF_SIZE 4096

int decodeVideo(const char *srcPath, const char *dstPath) {
    LOGI("Source file path: %s, dest file path: %s", srcPath, dstPath);

    const AVCodec *avCodec = nullptr;
    AVCodecParserContext *avCodecParserContext = nullptr;
    AVCodecContext *avCodecContext = nullptr;
    FILE *file = nullptr;
    AVFrame *avFrame = nullptr;
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data = nullptr;
    size_t data_size;
    int ret;
    AVPacket *avPacket = nullptr;

    avPacket = av_packet_alloc();
    if (!avPacket) {
//        goto end;
    }

    //原型：memset(void *buffer, int c, int count)
    //将inbuf[INBUF_SIZE]及其后面的元素都设为了0
    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    //找到h264的解码器
    avCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!avCodec) {
        LOGI("Codec not found");
//        goto end;
    }

    avCodecParserContext = av_parser_init(avCodec->id);
    if (!avCodecParserContext) {
        LOGI("avCodecParserContext not found\n");
//        goto end;
    }

    avCodecContext = avcodec_alloc_context3(avCodec);
    if (!avCodecContext) {
        LOGI("Could not allocate avCodecContext\n");
//        goto end;
    }

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

    /* open it */
    if (avcodec_open2(avCodecContext, avCodec, nullptr) < 0) {
        LOGI("Could not open avCodec\n");
//        goto end;
    }

    //打开输入文件
    file = fopen(srcPath, "rbe");
    if (!file) {
        LOGI("Could not open in file: %s", strerror(errno));
//        goto end;
    }

    avFrame = av_frame_alloc();
    if (!avFrame) {
        LOGI("Could not allocate video avFrame\n");
//        goto end;
    }

    FILE *ofile = fopen(dstPath, "wbe");
    if (!ofile) {
        LOGI("Could not open out file: %s", strerror(errno));
        goto end;
    }

    while (!feof(file)) {
        /* read raw data from the input file */
        data_size = fread(inbuf, 1, INBUF_SIZE, file);
        if (!data_size)
            break;

        /* use the avCodecParserContext to split the data into frames */
        data = inbuf;
        while (data_size > 0) {
            //从data中解析出avPacket数据
            ret = av_parser_parse2(avCodecParserContext, avCodecContext, &avPacket->data,
                                   &avPacket->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                LOGI("Error while parsing\n");
                goto end;
            }
            //后移数组指针并更新data_size
            data += ret;
            data_size -= ret;

            //解码avPacket
            if (avPacket->size)
                decode(avCodecContext, avFrame, avPacket, ofile);
        }
    }

    /* flush the decoder */
    decode(avCodecContext, avFrame, nullptr, ofile);

end:
    if (file) fclose(file);
    if (ofile) fclose(ofile);
    if (avCodecParserContext) av_parser_close(avCodecParserContext);
    if (avCodecContext) avcodec_free_context(&avCodecContext);
    if (avFrame) av_frame_free(&avFrame);
    if (avPacket) av_packet_free(&avPacket);

    return 0;
}

//从packet中解码出frame
static void decode(AVCodecContext *avCodecContext, AVFrame *avFrame, AVPacket *pkt,
                   FILE *ofile) {
    int ret;

    //将packet发送给codec
    ret = avcodec_send_packet(avCodecContext, pkt);
    if (ret < 0) {
        LOGI("Error sending a packet for decoding: %s\n", av_err2str(ret));
        return;
    }

    while (ret >= 0) {
        //解码出frame并存入avFrame参数
        ret = avcodec_receive_frame(avCodecContext, avFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        } else if (ret < 0) {
            LOGI("Error during decoding\n");
            return;
        }

        if (avCodecContext->frame_number % 30 == 0) {
            LOGI("saving avFrame %3d\n", avCodecContext->frame_number);
        }

        /* the picture is allocated by the decoder. no need to free it */
        yuv_save(avFrame, ofile);
    }
}

//将avFrame保存为yuv文件
static void yuv_save(AVFrame *avFrame, FILE *ofile) {
    int width = avFrame->width;
    int height = avFrame->height;
    for (int i = 0; i < height; i++)
        fwrite(avFrame->data[0] + i * avFrame->linesize[0], 1, width, ofile);
    for (int j = 0; j < height / 2; j++)
        fwrite(avFrame->data[1] + j * avFrame->linesize[1], 1, width / 2, ofile);
    for (int k = 0; k < height / 2; k++)
        fwrite(avFrame->data[2] + k * avFrame->linesize[2], 1, width / 2, ofile);
}

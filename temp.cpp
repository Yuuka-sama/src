#define NOMINMAX
#define SDL_MAIN_HANDLED

#include <iostream>
#include <windows.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <SDL.h>

// 音频队列
std::queue<std::vector<uint8_t>> audio_queue;
std::mutex audio_mutex;
std::condition_variable audio_cv;
bool quit = false;

void audio_callback(void* userdata, Uint8* stream, int len) {
    static bool first_call = true;
    if (first_call) {
        std::cerr << "Audio callback called first time!" << std::endl;
        first_call = false;
    }
    static int call_count = 0;
    call_count++;
    std::unique_lock<std::mutex> lock(audio_mutex);
    if (audio_queue.empty()) {
        SDL_memset(stream, 0, len);
        if (call_count % 100 == 0) {
            std::cerr << "Callback called, queue empty" << std::endl;
        }
        return;
    }
    if (call_count % 100 == 0) {
        std::cerr << "Callback called, queue size=" << audio_queue.size() << std::endl;
    }
    auto& buf = audio_queue.front();
    size_t copy_len = (len < buf.size()) ? len : buf.size();
    SDL_memcpy(stream, buf.data(), copy_len);
    if (buf.size() > copy_len) {
        buf.erase(buf.begin(), buf.begin() + copy_len);
    } else {
        audio_queue.pop();
    }
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <audio_file>" << std::endl;
        return -1;
    }
    const char* filename = argv[1];

    // 打开输入文件
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        std::cerr << "Could not open file: " << filename << std::endl;
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 找到音频流
    int audio_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        std::cerr << "No audio stream found" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecParameters* codecpar = fmt_ctx->streams[audio_stream_index]->codecpar;

    // 查找并打开解码器
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec!" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Could not allocate codec context" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        std::cerr << "Could not copy codec params to context" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 初始化 SDL 音频
    SDL_Init(SDL_INIT_AUDIO);
    SDL_AudioSpec desired, obtained;
    SDL_memset(&desired, 0, sizeof(desired));
    desired.freq = codec_ctx->sample_rate;
    desired.format = AUDIO_S16SYS;
    desired.channels = codec_ctx->ch_layout.nb_channels;
    desired.samples = 4096;
    desired.callback = audio_callback;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (dev == 0) {
        std::cerr << "SDL_OpenAudioDevice failed: " << SDL_GetError() << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        SDL_Quit();
        return -1;
    }

    std::cout << "Audio device opened: freq=" << obtained.freq
              << " channels=" << (int)obtained.channels
              << " format=" << obtained.format << std::endl;

    // 重采样上下文
    SwrContext* swr_ctx = nullptr;
    AVChannelLayout in_ch_layout = codec_ctx->ch_layout;
    AVSampleFormat in_sample_fmt = codec_ctx->sample_fmt;
    int in_sample_rate = codec_ctx->sample_rate;

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, obtained.channels);
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = obtained.freq;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        std::cerr << "Could not allocate resampler" << std::endl;
        SDL_CloseAudioDevice(dev);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        SDL_Quit();
        return -1;
    }
    av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", in_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", in_sample_fmt, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);
    if (swr_init(swr_ctx) < 0) {
        std::cerr << "Could not initialize resampler" << std::endl;
        swr_free(&swr_ctx);
        SDL_CloseAudioDevice(dev);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        SDL_Quit();
        return -1;
    }

    // 开始播放
    SDL_PauseAudioDevice(dev, 0);  // 此函数返回 void，无需判断
    std::cout << "Audio unpaused, playback started" << std::endl;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int frame_count = 0;   // 放在循环外

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            int ret = avcodec_send_packet(codec_ctx, packet);
            if (ret < 0) {
                std::cerr << "Error sending packet" << std::endl;
                break;
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "Error during decoding" << std::endl;
                    break;
                }

                frame_count++;
                if (frame_count <= 5) {
                    std::cout << "Decoded frame " << frame_count << ": samples=" << frame->nb_samples
                              << " sample_rate=" << frame->sample_rate
                              << " channels=" << frame->ch_layout.nb_channels
                              << " format=" << av_get_sample_fmt_name((AVSampleFormat)frame->format)
                              << std::endl;
                }

                // 重采样
                int dst_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples,
                    out_sample_rate, frame->sample_rate, AV_ROUND_UP);
                int max_out_bytes = av_samples_get_buffer_size(nullptr, out_ch_layout.nb_channels,
                    dst_nb_samples, out_sample_fmt, 1);
                std::vector<uint8_t> out_buf(max_out_bytes);
                uint8_t* out_ptr = out_buf.data();
                int converted = swr_convert(swr_ctx, &out_ptr, dst_nb_samples,
                    (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (converted < 0) {
                    std::cerr << "swr_convert error" << std::endl;
                    break;
                }

                if (frame_count <= 5) {
                    std::cout << "Converted " << converted << " samples, queue size before push: "
                              << audio_queue.size() << std::endl;
                }

                int out_bytes = av_samples_get_buffer_size(nullptr, out_ch_layout.nb_channels,
                    converted, out_sample_fmt, 1);

                {
                    std::lock_guard<std::mutex> lock(audio_mutex);
                    audio_queue.push(std::vector<uint8_t>(out_ptr, out_ptr + out_bytes));
                }
                audio_cv.notify_all();

                if (frame_count <= 5) {
                    std::cout << "Queue size after push: " << audio_queue.size() << std::endl;
                }
            }
        }
        av_packet_unref(packet);
    }

    // 等待队列播放完毕（简单等待，后续可优化）
    SDL_Delay(2000);

    // 清理
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    SDL_CloseAudioDevice(dev);
    SDL_Quit();

    return 0;
}
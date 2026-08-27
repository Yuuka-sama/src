#define NOMINMAX                            //防止windows.h中定义的min/max宏与std::min/std::max冲突
#define SDL_MAIN_HANDLED                    //防止SDL重定义main函数

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

// 音频队列（线程安全）
std::queue<std::vector<uint8_t>> audio_queue;
std::mutex audio_mutex;
std::condition_variable audio_cv;
const size_t MAX_QUEUE_SIZE = 100;

// 固定块大小：1024 帧 * 2 声道 * float(4字节) = 8192 字节
const size_t CHUNK_SIZE = 1024 * 2 * sizeof(float);

// 累积缓冲区
std::vector<uint8_t> pending_buffer;

// SDL 音频回调
//如队列为空，则填充静音数据；否则从队列中取出数据填充到 stream 中，并从队列中移除已使用的数据
//移除已复制数据，并通知主线程有空间
void audio_callback(void* userdata, Uint8* stream, int len) {
    std::unique_lock<std::mutex> lock(audio_mutex);
    if (audio_queue.empty()) {
        SDL_memset(stream, 0, len);
        return;
    }

    auto& buf = audio_queue.front();
    size_t copy_len = (len < buf.size()) ? len : buf.size();
    SDL_memcpy(stream, buf.data(), copy_len);

    // 如果数据不足，剩余部分填静音（正常情况应该恰好匹配）
    if (copy_len < (size_t)len) {
        SDL_memset(stream + copy_len, 0, len - copy_len);
    }

    if (buf.size() > copy_len) {
        buf.erase(buf.begin(), buf.begin() + copy_len);
    } else {
        audio_queue.pop();
    }

    // 通知主线程队列有空间了
    audio_cv.notify_one();
}



//主函数
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <audio_file>" << std::endl;
        return -1;
    }
    const char* filename = argv[1];

    // 1. 打开输入文件
    //使用FFmpeg打开媒体文件，获取信息流，找到第一个音频流
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        std::cerr << "Could not open file" << std::endl;
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 2. 找到音频流
    int audio_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        std::cerr << "No audio stream" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecParameters* codecpar = fmt_ctx->streams[audio_stream_index]->codecpar;

    // 3. 优先使用 mp3float 解码器
    const AVCodec* codec = avcodec_find_decoder_by_name("mp3float");
    if (!codec) codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Could not allocate codec context" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    avcodec_parameters_to_context(codec_ctx, codecpar);
    codec_ctx->thread_count = 1;  // 单线程解码，避免同步问题
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    std::cout << "Codec sample rate: " << codec_ctx->sample_rate
              << ", channels: " << codec_ctx->ch_layout.nb_channels
              << ", format: " << av_get_sample_fmt_name(codec_ctx->sample_fmt) << std::endl;

    // 4. 初始化 SDL 音频
    SDL_Init(SDL_INIT_AUDIO);
    SDL_AudioSpec desired, obtained;
    SDL_memset(&desired, 0, sizeof(desired));
    desired.freq = 44100;               //固定输出采样率为 44100 Hz
    desired.format = AUDIO_F32SYS;      //固定输出采样格式为 float
    desired.channels = 2;               //固定输出为双声道  
    desired.samples = 1024;             //回调请求的样本帧数
    desired.callback = audio_callback;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (dev == 0) {
        std::cerr << "SDL_OpenAudioDevice failed" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        SDL_Quit();
        return -1;
    }
    std::cout << "Audio device: freq=" << obtained.freq
              << ", channels=" << (int)obtained.channels
              << ", format=" << obtained.format << std::endl;

    // 5. 初始化重采样设置：固定输出 44100 Hz、双声道、FLT
    SwrContext* swr_ctx = nullptr;
    AVChannelLayout in_ch_layout = codec_ctx->ch_layout;
    AVSampleFormat in_sample_fmt = codec_ctx->sample_fmt;
    int in_sample_rate = codec_ctx->sample_rate;

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2);   // 双声道
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_FLT;
    int out_sample_rate = 44100;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        std::cerr << "Could not allocate resampler" << std::endl;
        exit(1);
    }
    av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", in_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", in_sample_fmt, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);
    if (swr_init(swr_ctx) < 0) {
        std::cerr << "swr_init failed" << std::endl;
        exit(1);
    }



    // 6. 开始播放
    SDL_PauseAudioDevice(dev, 0);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int frame_count = 0;

    int read_ret;
    //解码循环
    while ((read_ret = av_read_frame(fmt_ctx, packet)) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            int ret = avcodec_send_packet(codec_ctx, packet);
            if (ret < 0) break;
            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                else if (ret < 0) break;

                frame_count++;

                // 重采样
                int dst_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples,
                    out_sample_rate, frame->sample_rate, AV_ROUND_UP);
                std::vector<uint8_t> out_buf(
                    av_samples_get_buffer_size(nullptr, out_ch_layout.nb_channels,
                                               dst_nb_samples, out_sample_fmt, 1));
                uint8_t* out_ptr = out_buf.data();
                int converted = swr_convert(swr_ctx, &out_ptr, dst_nb_samples,
                                            (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (converted < 0) {
                    std::cerr << "swr_convert error" << std::endl;
                    break;
                }
                out_ptr = out_buf.data();   // 重置指针
                int out_bytes = av_samples_get_buffer_size(nullptr, out_ch_layout.nb_channels,
                                                           converted, out_sample_fmt, 1);

                // 将数据累积到 pending_buffer，并切成固定块推入队列
                std::unique_lock<std::mutex> lock(audio_mutex);
                pending_buffer.insert(pending_buffer.end(), out_ptr, out_ptr + out_bytes);

                while (pending_buffer.size() >= CHUNK_SIZE) {
                    // 如果队列已满，等待空间
                    audio_cv.wait(lock, [] { return audio_queue.size() < MAX_QUEUE_SIZE; });

                    std::vector<uint8_t> chunk(pending_buffer.begin(), pending_buffer.begin() + CHUNK_SIZE);
                    pending_buffer.erase(pending_buffer.begin(), pending_buffer.begin() + CHUNK_SIZE);
                    audio_queue.push(std::move(chunk));
                }
                // 注意：这里不需要 notify，消费者回调会 notify
            }
        }
        av_packet_unref(packet);
    }

    // 处理剩余不足一块的数据
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        if (!pending_buffer.empty()) {
            audio_queue.push(std::move(pending_buffer));
        }
    }

    // 等待队列清空
    {
        std::unique_lock<std::mutex> lock(audio_mutex);
        audio_cv.wait(lock, [] { return audio_queue.empty(); });
    }
    SDL_Delay(200);

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
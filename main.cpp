#define NOMINMAX                            //防止windows.h中定义的min/max宏与std::min/std::max冲突
#define SDL_MAIN_HANDLED                    //防止SDL重定义main函数

#include <iostream>
#include <windows.h>
#include <conio.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>

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

// 播放控制状态（由输入线程更新，解码线程读取）
std::atomic<bool> quit{ false };
std::atomic<bool> paused{ false };
std::atomic<bool> seek_requested{ false };
std::atomic<double> seek_target_seconds{ 0.0 };
std::atomic<uint64_t> total_written_bytes{ 0 };
size_t audio_queue_bytes = 0;   // 受 audio_mutex 保护
double g_bytes_per_second = 0.0;
double g_duration_seconds = 0.0;
const double SEEK_STEP_SECONDS = 10.0;

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
    audio_queue_bytes = (audio_queue_bytes >= copy_len) ? audio_queue_bytes - copy_len : 0;

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



double get_current_position_seconds() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    uint64_t written = total_written_bytes.load();
    size_t pending = audio_queue_bytes + pending_buffer.size();
    if (g_bytes_per_second <= 0.0) return 0.0;
    double pos = (written > pending) ? (double)(written - pending) / g_bytes_per_second : 0.0;
    return pos;
}

void request_seek(double delta_seconds) {
    double pos = get_current_position_seconds();
    double target = pos + delta_seconds;
    if (target < 0.0) target = 0.0;
    if (g_duration_seconds > 0.0 && target > g_duration_seconds) target = g_duration_seconds;
    seek_target_seconds.store(target);
    seek_requested.store(true);
    audio_cv.notify_all();
    std::cout << "[控制] 跳转到 " << target << " 秒" << std::endl;
}

void clear_audio_buffers_locked() {
    audio_queue = std::queue<std::vector<uint8_t>>();
    pending_buffer.clear();
    audio_queue_bytes = 0;
}

bool seek_to(AVFormatContext* fmt_ctx, AVCodecContext* codec_ctx, SwrContext* swr_ctx,
            int audio_stream_index, double seconds) {
    AVStream* stream = fmt_ctx->streams[audio_stream_index];
    int64_t ts = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE), AVRational{ 1, AV_TIME_BASE }, stream->time_base);
    if (ts < 0) ts = 0;
    if (stream->duration != AV_NOPTS_VALUE && ts > stream->duration) ts = stream->duration;
    int ret = av_seek_frame(fmt_ctx, audio_stream_index, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        std::cerr << "[控制] 跳转失败" << std::endl;
        return false;
    }
    avcodec_flush_buffers(codec_ctx);
    swr_close(swr_ctx);
    swr_init(swr_ctx);
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        clear_audio_buffers_locked();
        total_written_bytes.store(static_cast<uint64_t>(seconds * g_bytes_per_second));
    }
    std::cout << "[控制] 已跳转到 " << seconds << " 秒" << std::endl;
    return true;
}

void input_handler(SDL_AudioDeviceID dev) {
    while (!quit.load()) {
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'p' || ch == 'P' || ch == ' ') {
                if (!paused.load()) {
                    double pos = get_current_position_seconds();
                    paused.store(true);
                    SDL_PauseAudioDevice(dev, 1);
                    seek_target_seconds.store(pos);
                    seek_requested.store(true);
                    audio_cv.notify_all();
                    std::cout << "[控制] 已暂停 @ " << pos << " 秒" << std::endl;
                } else {
                    paused.store(false);
                    SDL_PauseAudioDevice(dev, 0);
                    audio_cv.notify_all();
                    std::cout << "[控制] 继续播放" << std::endl;
                }
            } else if (ch == 'f' || ch == 'F') {
                request_seek(SEEK_STEP_SECONDS);
            } else if (ch == 'b' || ch == 'B') {
                request_seek(-SEEK_STEP_SECONDS);
            } else if (ch == 'q' || ch == 'Q') {
                quit.store(true);
                SDL_PauseAudioDevice(dev, 1);
                audio_cv.notify_all();
                std::cout << "[控制] 退出播放" << std::endl;
                return;
            } else if (ch == 0 || ch == 224) {
                if (_kbhit()) {
                    int ch2 = _getch();
                    if (ch2 == 77) request_seek(SEEK_STEP_SECONDS);       // 右方向键
                    else if (ch2 == 75) request_seek(-SEEK_STEP_SECONDS); // 左方向键
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
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



    // 6. 设置播放控制所需参数
    g_bytes_per_second = obtained.freq * obtained.channels * sizeof(float);
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        g_duration_seconds = (double)fmt_ctx->duration / AV_TIME_BASE;
    } else if (fmt_ctx->streams[audio_stream_index]->duration != AV_NOPTS_VALUE) {
        g_duration_seconds = fmt_ctx->streams[audio_stream_index]->duration *
                             av_q2d(fmt_ctx->streams[audio_stream_index]->time_base);
    }

    std::cout << "\n控制键: P/空格=暂停/继续, F/→=快进10秒, B/←=快退10秒, Q=退出\n" << std::endl;

    // 7. 启动输入监听线程
    std::thread input_thread(input_handler, dev);

    // 8. 开始播放
    SDL_PauseAudioDevice(dev, 0);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int frame_count = 0;
    bool eof = false;

    while (!quit.load()) {
        // 处理跳转请求（暂停时也会通过跳转到当前位置来安全清空缓冲）
        if (seek_requested.exchange(false)) {
            double target = seek_target_seconds.load();
            if (seek_to(fmt_ctx, codec_ctx, swr_ctx, audio_stream_index, target)) {
                eof = false;
            }
            continue;
        }

        // 暂停时等待继续/跳转/退出
        if (paused.load()) {
            std::unique_lock<std::mutex> lock(audio_mutex);
            audio_cv.wait(lock, [] { return !paused.load() || seek_requested.load() || quit.load(); });
            continue;
        }

        // 已读到文件末尾：等待队列播完，或响应用户控制
        if (eof) {
            std::unique_lock<std::mutex> lock(audio_mutex);
            audio_cv.wait(lock, [] {
                return quit.load() || seek_requested.load() || paused.load() || audio_queue.empty();
            });
            if (quit.load() || seek_requested.load() || paused.load()) continue;
            break;  // 队列已清空，正常播放结束
        }

        int read_ret = av_read_frame(fmt_ctx, packet);
        if (read_ret < 0) {
            // 文件读取结束，把剩余不足一块的数据也送入队列
            {
                std::lock_guard<std::mutex> lock(audio_mutex);
                if (!pending_buffer.empty()) {
                    audio_queue_bytes += pending_buffer.size();
                    total_written_bytes.fetch_add(pending_buffer.size());
                    audio_queue.push(std::move(pending_buffer));
                }
            }
            eof = true;
            continue;
        }

        if (packet->stream_index != audio_stream_index) {
            av_packet_unref(packet);
            continue;
        }

        int ret = avcodec_send_packet(codec_ctx, packet);
        if (ret < 0) {
            av_packet_unref(packet);
            break;
        }
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
                // 如果队列已满，等待空间；暂停/跳转/退出时立即醒来
                audio_cv.wait(lock, [] {
                    return quit.load() || seek_requested.load() || paused.load() ||
                           audio_queue.size() < MAX_QUEUE_SIZE;
                });
                if (quit.load() || seek_requested.load() || paused.load()) break;

                std::vector<uint8_t> chunk(pending_buffer.begin(), pending_buffer.begin() + CHUNK_SIZE);
                pending_buffer.erase(pending_buffer.begin(), pending_buffer.begin() + CHUNK_SIZE);
                audio_queue_bytes += chunk.size();
                total_written_bytes.fetch_add(chunk.size());
                audio_queue.push(std::move(chunk));
            }

            if (quit.load() || seek_requested.load() || paused.load()) break;
        }
        av_packet_unref(packet);
        if (quit.load() || seek_requested.load() || paused.load()) continue;
    }

    // 退出时停止音频并清理队列
    SDL_PauseAudioDevice(dev, 1);
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        clear_audio_buffers_locked();
    }
    audio_cv.notify_all();

    quit.store(true);
    if (input_thread.joinable()) input_thread.join();

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
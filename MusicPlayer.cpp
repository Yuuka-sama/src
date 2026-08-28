#include "MusicPlayer.h"

#define NOMINMAX
#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

struct MusicPlayer::Impl {
    // ---------- 音频队列 ----------
    std::queue<std::vector<uint8_t>> audio_queue;
    std::mutex audio_mutex;
    std::condition_variable audio_cv;
    const size_t MAX_QUEUE_SIZE = 100;
    const size_t CHUNK_SIZE = 1024 * 2 * sizeof(float);

    // 累积缓冲区
    std::vector<uint8_t> pending_buffer;

    // ---------- 播放控制状态 ----------
    std::atomic<bool> quit{ false };
    std::atomic<bool> paused{ false };
    std::atomic<bool> seek_requested{ false };
    std::atomic<double> seek_target_seconds{ 0.0 };
    std::atomic<uint64_t> total_written_bytes{ 0 };

    // 受 audio_mutex 保护
    size_t audio_queue_bytes = 0;

    double bytes_per_second = 0.0;
    double duration_seconds = 0.0;

    // ---------- 播放资源 ----------
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    int audio_stream_index = -1;
    SDL_AudioDeviceID audio_device = 0;
    bool sdl_audio_inited = false;

    std::atomic<bool> opened{ false };
    std::atomic<bool> started{ false };
    std::atomic<bool> thread_running{ false };
    std::thread decode_thread;

    // ---------- 回调 ----------
    static void SDLAudioCallback(void* userdata, Uint8* stream, int len) {
        auto* self = static_cast<Impl*>(userdata);
        self->handleAudioCallback(stream, len);
    }

    void handleAudioCallback(Uint8* stream, int len) {
        std::unique_lock<std::mutex> lock(audio_mutex);
        if (audio_queue.empty()) {
            SDL_memset(stream, 0, len);
            return;
        }

        auto& buf = audio_queue.front();
        size_t copy_len = (len < buf.size()) ? len : buf.size();
        SDL_memcpy(stream, buf.data(), copy_len);
        audio_queue_bytes = (audio_queue_bytes >= copy_len) ? audio_queue_bytes - copy_len : 0;

        if (copy_len < (size_t)len) {
            SDL_memset(stream + copy_len, 0, len - copy_len);
        }

        if (buf.size() > copy_len) {
            buf.erase(buf.begin(), buf.begin() + copy_len);
        } else {
            audio_queue.pop();
        }

        audio_cv.notify_one();
    }

    // ---------- 工具方法 ----------
    void clearBuffersLocked() {
        audio_queue = std::queue<std::vector<uint8_t>>();
        pending_buffer.clear();
        audio_queue_bytes = 0;
    }

    double getPositionSeconds() {
        std::lock_guard<std::mutex> lock(audio_mutex);
        uint64_t written = total_written_bytes.load();
        size_t pending = audio_queue_bytes + pending_buffer.size();
        if (bytes_per_second <= 0.0) return 0.0;
        return (written > pending) ? (double)(written - pending) / bytes_per_second : 0.0;
    }

    void requestSeek(double target) {
        if (target < 0.0) target = 0.0;
        if (duration_seconds > 0.0 && target > duration_seconds) target = duration_seconds;
        seek_target_seconds.store(target);
        seek_requested.store(true);
        audio_cv.notify_all();
    }

    bool seekTo(double seconds) {
        if (!fmt_ctx || audio_stream_index < 0) return false;

        AVStream* stream = fmt_ctx->streams[audio_stream_index];
        int64_t ts = av_rescale_q(
            static_cast<int64_t>(seconds * AV_TIME_BASE),
            AVRational{ 1, AV_TIME_BASE },
            stream->time_base);
        if (ts < 0) ts = 0;
        if (stream->duration != AV_NOPTS_VALUE && ts > stream->duration) ts = stream->duration;

        int ret = av_seek_frame(fmt_ctx, audio_stream_index, ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            std::cerr << "[播放器] 跳转失败" << std::endl;
            return false;
        }

        avcodec_flush_buffers(codec_ctx);
        if (swr_ctx) {
            swr_close(swr_ctx);
            swr_init(swr_ctx);
        }

        {
            std::lock_guard<std::mutex> lock(audio_mutex);
            clearBuffersLocked();
            total_written_bytes.store(static_cast<uint64_t>(seconds * bytes_per_second));
        }

        return true;
    }

    void releaseMediaResources() {
        if (audio_device) {
            SDL_CloseAudioDevice(audio_device);
            audio_device = 0;
        }
        if (sdl_audio_inited) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_audio_inited = false;
        }
        if (swr_ctx) {
            swr_free(&swr_ctx);
            swr_ctx = nullptr;
        }
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
            codec_ctx = nullptr;
        }
        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);
            fmt_ctx = nullptr;
        }
    }

    // ---------- 解码线程 ----------
    void decodeLoop() {
        thread_running = true;
        started = true;

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool eof = false;

        while (!quit.load()) {
            if (seek_requested.exchange(false)) {
                double target = seek_target_seconds.load();
                if (seekTo(target)) {
                    eof = false;
                }
                continue;
            }

            if (paused.load()) {
                std::unique_lock<std::mutex> lock(audio_mutex);
                audio_cv.wait(lock, [this] {
                    return !paused.load() || seek_requested.load() || quit.load();
                });
                continue;
            }

            if (eof) {
                std::unique_lock<std::mutex> lock(audio_mutex);
                audio_cv.wait(lock, [this] {
                    return quit.load() || seek_requested.load() || paused.load() || audio_queue.empty();
                });
                if (quit.load() || seek_requested.load() || paused.load()) continue;
                break;
            }

            int read_ret = av_read_frame(fmt_ctx, packet);
            if (read_ret < 0) {
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

                // 重采样为固定的 44100 Hz / 双声道 / float
                int dst_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples,
                    44100, frame->sample_rate, AV_ROUND_UP);
                std::vector<uint8_t> out_buf(
                    av_samples_get_buffer_size(nullptr, 2, dst_nb_samples, AV_SAMPLE_FMT_FLT, 1));
                uint8_t* out_ptr = out_buf.data();
                int converted = swr_convert(
                    swr_ctx, &out_ptr, dst_nb_samples,
                    (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (converted < 0) {
                    std::cerr << "swr_convert error" << std::endl;
                    break;
                }
                out_ptr = out_buf.data();
                int out_bytes = av_samples_get_buffer_size(
                    nullptr, 2, converted, AV_SAMPLE_FMT_FLT, 1);

                std::unique_lock<std::mutex> lock(audio_mutex);
                pending_buffer.insert(pending_buffer.end(), out_ptr, out_ptr + out_bytes);

                while (pending_buffer.size() >= CHUNK_SIZE) {
                    audio_cv.wait(lock, [this] {
                        return quit.load() || seek_requested.load() || paused.load() ||
                               audio_queue.size() < MAX_QUEUE_SIZE;
                    });
                    if (quit.load() || seek_requested.load() || paused.load()) break;

                    std::vector<uint8_t> chunk(
                        pending_buffer.begin(), pending_buffer.begin() + CHUNK_SIZE);
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

        av_frame_free(&frame);
        av_packet_free(&packet);

        thread_running = false;
        started = false;
    }
};

MusicPlayer::MusicPlayer() : impl_(std::make_unique<Impl>()) {}

MusicPlayer::~MusicPlayer() {
    close();
}

bool MusicPlayer::open(const std::string& filepath) {
    if (!impl_) return false;
    close();

    Impl& d = *impl_;
    const char* filename = filepath.c_str();

    d.fmt_ctx = nullptr;
    d.codec_ctx = nullptr;
    d.swr_ctx = nullptr;
    d.audio_stream_index = -1;
    d.audio_device = 0;
    d.sdl_audio_inited = false;
    d.opened = false;
    d.started = false;
    d.thread_running = false;
    d.quit = false;
    d.paused = false;
    d.seek_requested = false;
    d.total_written_bytes = 0;
    d.audio_queue_bytes = 0;
    d.bytes_per_second = 0.0;
    d.duration_seconds = 0.0;

    // 1. 打开输入文件
    if (avformat_open_input(&d.fmt_ctx, filename, nullptr, nullptr) < 0) {
        std::cerr << "Could not open file: " << filename << std::endl;
        d.releaseMediaResources();
        return false;
    }
    if (avformat_find_stream_info(d.fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info" << std::endl;
        d.releaseMediaResources();
        return false;
    }

    // 2. 找到音频流
    for (unsigned int i = 0; i < d.fmt_ctx->nb_streams; i++) {
        if (d.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            d.audio_stream_index = i;
            break;
        }
    }
    if (d.audio_stream_index == -1) {
        std::cerr << "No audio stream found" << std::endl;
        d.releaseMediaResources();
        return false;
    }

    // 3. 打开解码器（MP3 优先使用 mp3float）
    AVCodecParameters* codecpar = d.fmt_ctx->streams[d.audio_stream_index]->codecpar;
    const AVCodec* codec = nullptr;
    if (codecpar->codec_id == AV_CODEC_ID_MP3) {
        codec = avcodec_find_decoder_by_name("mp3float");
    }
    if (!codec) codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec" << std::endl;
        d.releaseMediaResources();
        return false;
    }

    d.codec_ctx = avcodec_alloc_context3(codec);
    if (!d.codec_ctx) {
        std::cerr << "Could not allocate codec context" << std::endl;
        d.releaseMediaResources();
        return false;
    }
    if (avcodec_parameters_to_context(d.codec_ctx, codecpar) < 0) {
        std::cerr << "Could not copy codec params" << std::endl;
        d.releaseMediaResources();
        return false;
    }
    d.codec_ctx->thread_count = 1;
    if (avcodec_open2(d.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec" << std::endl;
        d.releaseMediaResources();
        return false;
    }

    // 4. 初始化 SDL 音频
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL_InitSubSystem failed: " << SDL_GetError() << std::endl;
        d.releaseMediaResources();
        return false;
    }
    d.sdl_audio_inited = true;

    SDL_AudioSpec desired, obtained;
    SDL_memset(&desired, 0, sizeof(desired));
    desired.freq = 44100;
    desired.format = AUDIO_F32SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = &Impl::SDLAudioCallback;
    desired.userdata = &d;

    d.audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (d.audio_device == 0) {
        std::cerr << "SDL_OpenAudioDevice failed: " << SDL_GetError() << std::endl;
        d.releaseMediaResources();
        return false;
    }

    // 5. 初始化重采样器
    d.swr_ctx = swr_alloc();
    if (!d.swr_ctx) {
        std::cerr << "Could not allocate resampler" << std::endl;
        d.releaseMediaResources();
        return false;
    }

    AVChannelLayout in_ch_layout = d.codec_ctx->ch_layout;
    AVSampleFormat in_sample_fmt = d.codec_ctx->sample_fmt;
    int in_sample_rate = d.codec_ctx->sample_rate;

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2);

    av_opt_set_chlayout(d.swr_ctx, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_int(d.swr_ctx, "in_sample_rate", in_sample_rate, 0);
    av_opt_set_sample_fmt(d.swr_ctx, "in_sample_fmt", in_sample_fmt, 0);
    av_opt_set_chlayout(d.swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(d.swr_ctx, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(d.swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(d.swr_ctx) < 0) {
        std::cerr << "swr_init failed" << std::endl;
        d.releaseMediaResources();
        return false;
    }

    // 6. 记录时长和字节率，供进度/跳转使用
    d.bytes_per_second = obtained.freq * obtained.channels * sizeof(float);
    if (d.fmt_ctx->duration != AV_NOPTS_VALUE) {
        d.duration_seconds = (double)d.fmt_ctx->duration / AV_TIME_BASE;
    } else if (d.fmt_ctx->streams[d.audio_stream_index]->duration != AV_NOPTS_VALUE) {
        d.duration_seconds = d.fmt_ctx->streams[d.audio_stream_index]->duration *
                             av_q2d(d.fmt_ctx->streams[d.audio_stream_index]->time_base);
    }

    d.opened = true;
    return true;
}

void MusicPlayer::close() {
    if (!impl_) return;
    stop();
    impl_->releaseMediaResources();
    impl_->opened = false;
}

void MusicPlayer::play() {
    if (!impl_ || !impl_->opened) return;

    Impl& d = *impl_;

    if (d.thread_running.load()) {
        // 已在线程中，可能是暂停状态，直接恢复
        d.paused = false;
        SDL_PauseAudioDevice(d.audio_device, 0);
        d.audio_cv.notify_all();
        return;
    }

    // 如果之前的线程已经结束，先回收
    if (d.decode_thread.joinable()) {
        d.decode_thread.join();
    }

    d.quit = false;
    d.paused = false;
    d.started = true;
    d.thread_running = true;
    d.decode_thread = std::thread([this] { impl_->decodeLoop(); });
    SDL_PauseAudioDevice(d.audio_device, 0);
}

void MusicPlayer::pause() {
    if (!impl_ || !impl_->opened || !impl_->started) return;

    Impl& d = *impl_;
    if (!d.paused.load()) {
        double pos = d.getPositionSeconds();
        d.paused = true;
        SDL_PauseAudioDevice(d.audio_device, 1);
        d.seek_target_seconds.store(pos);
        d.seek_requested = true;
        d.audio_cv.notify_all();
    } else {
        d.paused = false;
        SDL_PauseAudioDevice(d.audio_device, 0);
        d.audio_cv.notify_all();
    }
}

void MusicPlayer::stop() {
    if (!impl_) return;

    Impl& d = *impl_;
    d.quit = true;
    if (d.audio_device) {
        SDL_PauseAudioDevice(d.audio_device, 1);
    }
    d.audio_cv.notify_all();

    if (d.decode_thread.joinable()) {
        d.decode_thread.join();
    }

    d.thread_running = false;
    d.started = false;
    d.paused = false;
    d.seek_requested = false;

    {
        std::lock_guard<std::mutex> lock(d.audio_mutex);
        d.clearBuffersLocked();
    }
}

void MusicPlayer::seek(double seconds) {
    if (!impl_ || !impl_->opened) return;
    impl_->requestSeek(seconds);
}

void MusicPlayer::seekBy(double deltaSeconds) {
    if (!impl_ || !impl_->opened) return;
    impl_->requestSeek(impl_->getPositionSeconds() + deltaSeconds);
}

bool MusicPlayer::isPaused() const {
    return impl_ && impl_->paused.load();
}

bool MusicPlayer::isPlaying() const {
    return impl_ && impl_->opened && impl_->thread_running.load() && !impl_->quit.load();
}

double MusicPlayer::getPosition() const {
    return impl_ ? impl_->getPositionSeconds() : 0.0;
}

double MusicPlayer::getDuration() const {
    return impl_ ? impl_->duration_seconds : 0.0;
}

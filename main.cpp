#include <iostream>
#include <windows.h>

<<<<<<< HEAD

=======
// 必须在包含 SDL.h 之前定义
#define SDL_MAIN_HANDLED

// FFmpeg 头文件
>>>>>>> 8743f73ccb4200aa88df850e276f6af8dc6b1ed1
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

<<<<<<< HEAD


int main(int argc, char* argv[]) {
=======
// SDL2 头文件
#include <SDL.h>

int main() {
>>>>>>> 8743f73ccb4200aa88df850e276f6af8dc6b1ed1
    // 设置控制台输出为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

<<<<<<< HEAD
    if(argc <2){
        std::cerr << "Usage: " << argv[0] << " <input_file>" << std::endl;
        return -1;
    }
    const char* filename = argv[1];

     // 打开输入文件
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        std::cerr << "Could not open file: " << filename << std::endl;      //无法打开文件
        return -1;
    }

    // 获取流信息
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 遍历流，找到音频流
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

    std::cout << "Audio stream index: " << audio_stream_index << std::endl;
    std::cout << "Codec ID: " << avcodec_get_name(codecpar->codec_id) << std::endl;
    std::cout << "Sample rate: " << codecpar->sample_rate << " Hz" << std::endl;
    std::cout << "Channels: " << codecpar->ch_layout.nb_channels << std::endl;
    std::cout << "Bit rate: " << codecpar->bit_rate << " bps" << std::endl;

    avformat_close_input(&fmt_ctx);
    return 0;
}

}


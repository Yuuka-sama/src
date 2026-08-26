#include <iostream>
#include <windows.h>

// 必须在包含 SDL.h 之前定义
#define SDL_MAIN_HANDLED

// FFmpeg 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// SDL2 头文件
#include <SDL.h>

int main() {
    // 设置控制台输出为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 打印 FFmpeg 版本
    std::cout << "FFmpeg version: " << av_version_info() << std::endl;

    // 初始化 SDL 音频
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return -1;
    }
    std::cout << "SDL audio initialized successfully" << std::endl;

    SDL_Quit();
    std::cout << "Test completed!" << std::endl;
    return 0;
}
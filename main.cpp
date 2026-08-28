#include "MusicPlayer.h"

#include <iostream>
#include <windows.h>
#include <conio.h>
#include <thread>
#include <atomic>
#include <chrono>

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <audio_file>" << std::endl;
        return -1;
    }

    MusicPlayer player;
    if (!player.open(argv[1])) {
        std::cerr << "Failed to open audio file: " << argv[1] << std::endl;
        return -1;
    }

    std::cout << "\n控制键: P/空格=暂停/继续, F/→=快进10秒, B/←=快退10秒, Q=退出\n" << std::endl;

    std::atomic<bool> quit{ false };

    // 输入监听线程：把键盘操作转换为 MusicPlayer 的公开接口调用
    std::thread input_thread([&] {
        while (!quit.load()) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 'p' || ch == 'P' || ch == ' ') {
                    if (player.isPaused()) {
                        player.play();
                        std::cout << "[控制] 继续播放" << std::endl;
                    } else {
                        player.pause();
                        std::cout << "[控制] 已暂停 @ " << player.getPosition() << " 秒" << std::endl;
                    }
                } else if (ch == 'f' || ch == 'F') {
                    player.seekBy(10.0);
                    std::cout << "[控制] 快进，当前 " << player.getPosition() << " 秒" << std::endl;
                } else if (ch == 'b' || ch == 'B') {
                    player.seekBy(-10.0);
                    std::cout << "[控制] 快退，当前 " << player.getPosition() << " 秒" << std::endl;
                } else if (ch == 'q' || ch == 'Q') {
                    quit = true;
                    player.stop();
                    std::cout << "[控制] 退出播放" << std::endl;
                    return;
                } else if (ch == 0 || ch == 224) {
                    if (_kbhit()) {
                        int ch2 = _getch();
                        if (ch2 == 77) {
                            player.seekBy(10.0);
                            std::cout << "[控制] 快进，当前 " << player.getPosition() << " 秒" << std::endl;
                        } else if (ch2 == 75) {
                            player.seekBy(-10.0);
                            std::cout << "[控制] 快退，当前 " << player.getPosition() << " 秒" << std::endl;
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    player.play();

    // 主线程等待播放结束或用户退出
    while (!quit.load() && player.isPlaying()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    quit = true;
    if (input_thread.joinable()) {
        input_thread.join();
    }

    player.close();
    return 0;
}
#pragma once

#include <memory>
#include <string>

// 音乐播放器封装类
// 负责 FFmpeg 解码、重采样、SDL 音频输出以及播放控制。
// GUI/CLI 只需要调用公开方法，不需要了解底层实现。
class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();

    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // 打开音频文件并初始化播放资源。成功返回 true。
    bool open(const std::string& filepath);

    // 释放所有播放资源。
    void close();

    // 开始播放；若当前处于暂停状态，则继续播放。
    void play();

    // 暂停播放；再次调用 play() 可继续。
    void pause();

    // 停止播放并清空缓冲，但保留已打开的文件资源。
    void stop();

    // 跳转到指定时间（秒）。
    void seek(double seconds);

    // 相对跳转，正数为快进，负数为快退。
    void seekBy(double deltaSeconds);

    bool isPaused() const;
    bool isPlaying() const;

    // 当前播放位置（秒）。
    double getPosition() const;

    // 音频总时长（秒），未知时返回 0。
    double getDuration() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

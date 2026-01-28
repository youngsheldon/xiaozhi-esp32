#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <map>
#include <esp_jpeg_dec.h>
#include <lvgl.h>
#include "music.h"
#include "esp_audio_simple_player.h"

struct JpegData
{
    uint8_t *buf;
    size_t len;
};

#define IMG_JPEG_BUF_SIZE 72 * 1024

class Esp32Music : public Music
{
private:
    std::string current_music_url_;
    std::string current_song_name_;
    std::string current_song_id_;
    bool song_name_displayed_;
    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_; // 时间戳和歌词文本
    std::mutex lyrics_mutex_;                         // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    // 歌曲推荐相关
    std::string play_next_;
    std::map<std::string, std::pair<bool, std::string>> song_played_map_; // 保存歌曲ID和歌曲名的映射关系
    std::vector<std::pair<std::string, std::string>> liked_songs_;
    int like_index_;
    // 音频播放相关
    esp_asp_handle_t asp_handle_;
    std::condition_variable play_next_cv_;
    std::mutex next_mutex_;
    bool need_to_play_next_;
    std::atomic<int64_t> current_play_time_ms_; // 当前播放时间（毫秒，累加得到）
    std::atomic<int> audio_sample_rate_;        // 音频采样率（如44100Hz，从事件中获取）
    std::atomic<int> audio_channels_;           // 音频声道数（如2，从事件中获取）
    std::mutex audio_param_mutex_;              // 保护音频参数的互斥锁
    bool is_audio_param_ready_;                 // 音频参数（采样率、声道数）是否就绪

    void DownloadAudioStream(const std::string &music_url);
    void ResetSampleRate();
    void PlayNextDetect();
    void AfterPlayDone();
    void setBackgroundImage(const std::string &albumId);
    bool Request(const std::string &url, std::string &response);
    bool ParseLyrics(const std::string &lyric_content);
    bool ParseRecommondSong(const std::string &keyword, const std::string &songId);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    std::string getSongPlayUrl(const std::string &req);

public:
    Esp32Music();
    ~Esp32Music();

    void CalculateFrameDuration(int sample_count);
    void SetAudioParam(int sample_rate, int channels)
    {
        std::lock_guard<std::mutex> lock(audio_param_mutex_);
        audio_sample_rate_ = sample_rate;
        audio_channels_ = channels;
        is_audio_param_ready_ = true;
    }
    void ResetAudioParam()
    {
        current_play_time_ms_ = 2000;
        is_audio_param_ready_ = false;
    }
    bool IsAudioParamReady() const { return is_audio_param_ready_; }
    int GetAudioSampleRate() const { return audio_sample_rate_; }
    int GetAudioChannels() const { return audio_channels_; }
    void OnAudioEvent(esp_asp_state_t st);
    
    virtual void TogglePlayState() override;
    virtual bool ClearRecommendedSongs() override;
    virtual bool AddLikedSong() override;
    virtual bool playNextSong() override;
    virtual bool Download(const std::string &song_name) override;
    virtual bool StartStreaming(const std::string &music_url) override;
    virtual bool StopStreaming() override;
};

#endif // ESP32_MUSIC_H

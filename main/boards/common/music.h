#ifndef MUSIC_H
#define MUSIC_H

#include <string>

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool ClearRecommendedSongs() = 0;
    virtual bool AddLikedSong() = 0;
    virtual bool playNextSong() = 0;
    virtual bool Download(const std::string& song_name) = 0;
    
    // 新增流式播放相关方法
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual void TogglePlayState() = 0;  // 播放/暂停
};

#endif // MUSIC_H 
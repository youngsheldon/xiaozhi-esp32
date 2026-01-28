#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype> // 为isdigit函数
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "KwWork.h"
#include "settings.h"
#include "lvgl_display.h"

#define TAG "Esp32Music"

// URL编码函数
static std::string url_encode(const std::string &str)
{
    std::string encoded;
    char hex[4];

    for (size_t i = 0; i < str.length(); i++)
    {
        unsigned char c = str[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else if (c == ' ')
        {
            encoded += '+'; // 空格编码为'+'或'%20'
        }
        else
        {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

static int out_data_callback(uint8_t *data, int data_size, void *ctx)
{
    if (ctx)
    {
        Esp32Music *music = (Esp32Music *)ctx;
        if (music->IsAudioParamReady())
        {
            const int bytes_per_sample = 2; // 16-bit PCM
            int sample_count = data_size / (bytes_per_sample * music->GetAudioChannels());
            // 计算并累加当前帧的播放时间
            music->CalculateFrameDuration(sample_count);
        }
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled())
    {
        codec->EnableOutput(true);
    }

    auto &app = Application::GetInstance();

    // 计算正确的样本数（16位双声道转单声道）
    int total_samples = data_size / sizeof(int16_t); // 总采样数（双声道）
    if (total_samples % 2 != 0)
    {
        ESP_LOGE(TAG, "Invalid PCM data size (not even)");
        return 0;
    }
    int mono_samples = total_samples / 2; // 单声道样本数
    if (mono_samples <= 0)
    {
        return data_size; // 无数据可处理
    }

    // 转换并混合声道（使用栈上缓冲区优化）
    std::vector<int16_t> mono_buffer(mono_samples);
    int16_t *pcm_16 = reinterpret_cast<int16_t *>(data);
    for (int i = 0; i < mono_samples; ++i)
    {
        int16_t left = pcm_16[i * 2];
        int16_t right = pcm_16[i * 2 + 1];
        mono_buffer[i] = static_cast<int16_t>((left + right) / 2);
    }

    // 填充音频包（尽量复用内存或使用对象池）
    AudioStreamPacket packet;
    packet.sample_rate = 48000;
    packet.frame_duration = 60;
    packet.timestamp = 0;
    packet.payload.resize(mono_samples * sizeof(int16_t));
    memcpy(packet.payload.data(), mono_buffer.data(), packet.payload.size());

    app.AddAudioData(std::move(packet));

    return data_size; // 返回实际处理的字节数
}

static int mock_event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO)
    {
        esp_asp_music_info_t info = {0};
        memcpy(&info, event->payload, event->payload_size);
        ESP_LOGW(TAG, "Get info, rate:%d, channels:%d, bits:%d", info.sample_rate, info.channels, info.bits);
        if (ctx)
        {
            Esp32Music *music = (Esp32Music *)ctx;
            music->SetAudioParam(info.sample_rate, info.channels);
        }
    }
    else if (event->type == ESP_ASP_EVENT_TYPE_STATE)
    {
        esp_asp_state_t st = ESP_ASP_STATE_NONE;
        memcpy(&st, event->payload, event->payload_size);
        ESP_LOGW(TAG, "Get State, %d,%s", st, esp_audio_simple_player_state_to_str(st));
        if (ctx)
        {
            Esp32Music *music = (Esp32Music *)ctx;
            music->OnAudioEvent(st);
        }
        if (st == ESP_ASP_STATE_STOPPED || st == ESP_ASP_STATE_FINISHED)
        {
            if (ctx)
            {
                Esp32Music *music = (Esp32Music *)ctx;
                music->ResetAudioParam();
            }
        }
    }
    return 0;
}

Esp32Music::Esp32Music() : current_music_url_(), current_song_name_(), current_song_id_(),
                           song_name_displayed_(false), current_lyric_url_(), lyrics_(),
                           current_lyric_index_(-1), play_next_(), song_played_map_(),
                           liked_songs_(), like_index_(0), asp_handle_(), play_next_cv_(),
                           next_mutex_(), need_to_play_next_(false), current_play_time_ms_(2000),
                           audio_sample_rate_(0), audio_channels_(0), audio_param_mutex_(), is_audio_param_ready_(false)

{
    ESP_LOGI(TAG, "Music player initialized");
    std::thread(&Esp32Music::PlayNextDetect, this).detach();
}

Esp32Music::~Esp32Music()
{
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    if (asp_handle_)
    {
        assert(ESP_OK == esp_audio_simple_player_destroy(asp_handle_));
        asp_handle_ = NULL;
    }

    ESP_LOGI(TAG, "Music player destroyed successfully");
}

void Esp32Music::PlayNextDetect()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(next_mutex_);
        // 等待直到 need_to_play_next_ 为 true
        play_next_cv_.wait(lock, [this]
                           { return need_to_play_next_; });
        need_to_play_next_ = false;
        // 释放锁，允许其他线程修改 need_to_play_next_
        lock.unlock();
        playNextSong();
        // 重新获取锁
        lock.lock();
        // 检查是否有新的播放请求
        if (need_to_play_next_)
        {
            continue; // 如果有新的请求，直接跳过重置步骤
        }
    }
}

void Esp32Music::CalculateFrameDuration(int sample_count)
{
    // 1. 原子变量读取：明确内存序，确保值有效
    int sample_rate = audio_sample_rate_.load(std::memory_order_acquire); // 增强内存序，确保读取最新值
    int channels = audio_channels_.load(std::memory_order_acquire);
    if (sample_rate == 0 || channels == 0 || sample_count == 0)
    {
        ESP_LOGW(TAG, "Invalid audio param: rate=%d, ch=%d, samples=%d", sample_rate, channels, sample_count);
        return;
    }

    // 2. 计算单帧时长：用 1000LL 确保 64 位运算，避免溢出
    int64_t frame_duration_ms = (static_cast<int64_t>(sample_count) * 2000LL) / (static_cast<int64_t>(sample_rate) * channels);
    if (frame_duration_ms <= 0)
    {
        ESP_LOGW(TAG, "Invalid frame duration: %lldms (samples=%d)", frame_duration_ms, sample_count);
        return;
    }

    // 3. 累加播放时间：原子操作，避免数据竞争
    current_play_time_ms_.fetch_add(frame_duration_ms, std::memory_order_release);

    // 4. 修复帧计数与日志打印：
    // - frame_cnt 改为 uint32_t 避免溢出；
    // - 显式读取原子变量，用 %lld 格式化
    static uint32_t frame_cnt = 0; // 改为无符号32位，支持更大计数（0~4294967295）
    if (++frame_cnt % 100 == 0)
    { // 每100帧打印一次，避免日志刷屏
        int64_t current_time = current_play_time_ms_.load(std::memory_order_acquire);
        // 关键：%lld 对应 int64_t，%u 对应 uint32_t
        ESP_LOGI(TAG, "Current play time: %d ms ", (int)current_time);
        UpdateLyricDisplay(current_time);
    }
}

void Esp32Music::OnAudioEvent(esp_asp_state_t st)
{
    if (st == ESP_ASP_STATE_FINISHED)
    {
        AfterPlayDone();
    }
}

bool Esp32Music::Download(const std::string &song_name)
{
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());

    // 保存歌名用于后续显示
    current_song_name_ = song_name;
    // 第一步：请求stream_pcm接口获取音频信息
    std::string full_url = "https://search.kuwo.cn/r.s?pn=0&rn=3&all=" + url_encode(song_name) + "&ft=music&newsearch=1&alflac=1&itemset=web_2013&client=kt&cluster=0&vermerge=1&rformat=json&encoding=utf8&show_copyright_off=1&pcmp4=1&ver=mbox&plat=pc&vipver=MUSIC_9.1.1.2_BCS2&devid=38668888&newver=1&issubtitle=1&pcjson=1";
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());

    // 使用Board提供的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    // 打开GET连接
    if (!http->Open("GET", full_url))
    {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return false;
    }

    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }

    // 读取响应数据
    std::string body(30720, '\0');
    int bytes_read = http->Read(body.data(), 30720);
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, bytes_read);
    http->Close();
    if (bytes_read > 0)
    {
        std::string last_downloaded_data_ = body.substr(0, bytes_read);
        // 解析响应JSON以提取音频URL
        cJSON *response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (!response_json)
        {
            return false;
        }

        // 获取abslist数组
        cJSON *abslist = cJSON_GetObjectItem(response_json, "abslist");
        if (!cJSON_IsArray(abslist) || cJSON_GetArraySize(abslist) == 0)
        {
            ESP_LOGE(TAG, "can not get song info from json!");
            cJSON_Delete(response_json);
            return false;
        }

        // 获取第一个歌曲对象
        cJSON *firstSong = cJSON_GetArrayItem(abslist, 0);
        cJSON *targetId = cJSON_GetObjectItem(firstSong, "DC_TARGETID");
        cJSON *artist = cJSON_GetObjectItem(firstSong, "ARTIST");
        cJSON *songName = cJSON_GetObjectItem(firstSong, "SONGNAME");
        cJSON *albumId = cJSON_GetObjectItem(firstSong, "ALBUMID");
        if (!targetId || !targetId->valuestring)
        {
            ESP_LOGE(TAG, "未找到歌曲ID");
            cJSON_Delete(response_json);
            return false;
        }

        ESP_LOGI(TAG, "歌曲ID: %s", targetId->valuestring);
        string songId = string(targetId->valuestring);
        string artistName = string(artist->valuestring);
        string songNameStr = string(songName->valuestring);
        string albumIdStr = string(albumId->valuestring);
        ESP_LOGI(TAG, "歌曲名称: %s", songNameStr.c_str());
        string url = KwWork::getUrl(songId);
        ESP_LOGI(TAG, "url = %s", url.c_str());
        cJSON_Delete(response_json);
        // 打开GET连接
        current_song_id_ = songId;
        current_music_url_ = this->getSongPlayUrl(url);
        if (current_music_url_.empty())
        {
            ESP_LOGE(TAG, "Failed to get song play url");
            return false;
        }
        ESP_LOGI(TAG, "songUrl = %s", current_music_url_.c_str());
        ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());

        std::thread(&Esp32Music::StartStreaming, this, current_music_url_).detach();

        // 处理歌词URL
        if (!songId.empty())
        {
            current_lyric_url_ = "https://www.kuwo.cn/openapi/v1/www/lyric/getlyric?musicId=" + songId;
            ESP_LOGI(TAG, "Loading lyrics for: %s", song_name.c_str());
            current_lyric_index_ = -1;
            lyrics_.clear();
            auto lcd = Board::GetInstance().GetDisplay();
            if (lcd)
            {
                lcd->SetPlayMusicStatus(true);
            }

            // 配置线程栈大小以避免栈溢出
            esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
            cfg.stack_size = 8192; // 8KB栈大小
            cfg.prio = 6;          // 中等优先级
            cfg.thread_name = "PlayAudio";
            esp_pthread_set_cfg(&cfg);

            std::thread(&Esp32Music::setBackgroundImage, this, albumIdStr).detach();
            std::thread(&Esp32Music::LyricDisplayThread, this).detach();
            std::thread(&Esp32Music::ParseRecommondSong, this, artistName, songId).detach();
        }
        else
        {
            ESP_LOGW(TAG, "No lyric URL found for this song");
        }
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "Empty response from music API");
    }

    return false;
}

bool Esp32Music::playNextSong()
{
    std::string albumIdStr = "";
    if (song_played_map_.empty())
    {
        if (liked_songs_.empty())
        {
            Settings settings("liked_songs", true);
            std::string liked_songs_str = settings.GetString("songs", liked_songs_str);
            ESP_LOGW(TAG, "liked_songs_str: %s", liked_songs_str.c_str());
            cJSON *json_obj = cJSON_Parse(liked_songs_str.c_str());
            if (!json_obj)
            {
                ESP_LOGE(TAG, "can not parse liked songs json!");
                cJSON_Delete(json_obj);
                return false;
            }
            cJSON *songs = cJSON_GetObjectItem(json_obj, "songs");
            if (!cJSON_IsArray(songs) || cJSON_GetArraySize(songs) == 0)
            {
                ESP_LOGE(TAG, "can not get song info from json!");
                cJSON_Delete(json_obj);
                return false;
            }
            for (int i = 0; i < cJSON_GetArraySize(songs); i++)
            {
                cJSON *song = cJSON_GetArrayItem(songs, i);
                cJSON *songId = cJSON_GetObjectItem(song, "songId");
                cJSON *albumId = cJSON_GetObjectItem(song, "albumId");
                liked_songs_.push_back(std::make_pair(std::string(songId->valuestring), std::string(albumId->valuestring)));
            }
            cJSON_Delete(json_obj);
        }
        play_next_ = liked_songs_[like_index_].first;
        albumIdStr = liked_songs_[like_index_].second;
        like_index_ = (like_index_ + 1) % liked_songs_.size();
    }

    bool next_song_found = false;
    if (!song_played_map_.empty())
    {
        std::lock_guard<std::mutex> lock(next_mutex_);
        for (const auto &elem : song_played_map_)
        {
            if (elem.second.first == false)
            {
                play_next_ = elem.first;
                albumIdStr = elem.second.second;
                break;
            }
        }
        if (next_song_found)
        {
            ESP_LOGI(TAG, "LyricDisplayThread: play next song: %s", play_next_.c_str());
        }
    }

    if (play_next_.empty())
    {
        ESP_LOGE(TAG, "song_id is empty");
        return false;
    }

    // 配置线程栈大小以避免栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192; // 8KB栈大小
    cfg.prio = 6;          // 中等优先级
    cfg.thread_name = "setBackgroundImage";
    esp_pthread_set_cfg(&cfg);

    std::thread(&Esp32Music::setBackgroundImage, this, albumIdStr).detach();

    ESP_LOGI(TAG, "歌曲ID: %s", play_next_.c_str());
    string url = KwWork::getUrl(play_next_);
    ESP_LOGI(TAG, "url = %s", url.c_str());
    current_song_id_ = play_next_;
    current_music_url_ = this->getSongPlayUrl(url);
    if (current_music_url_.empty())
    {
        ESP_LOGE(TAG, "Failed to get song play url");
        return false;
    }

    if (!song_played_map_.empty())
    {
        song_played_map_[play_next_].first = true;
    }
    ESP_LOGI(TAG, "songUrl = %s", current_music_url_.c_str());
    std::thread(&Esp32Music::StartStreaming, this, current_music_url_).detach();

    current_lyric_url_ = "https://www.kuwo.cn/openapi/v1/www/lyric/getlyric?musicId=" + play_next_;
    current_lyric_index_ = -1;
    lyrics_.clear();
    std::thread(&Esp32Music::LyricDisplayThread, this).detach();
    return true;
}

std::string Esp32Music::getSongPlayUrl(const std::string &req)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", req))
    {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return "";
    }

    // 检查响应状态码
    int status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return "";
    }
    // 读取响应数据
    std::string rsp = "";
    std::string body(1024, '\0');
    int bytes_read = http->Read(body.data(), 1024);
    http->Close();
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d, body = %s", status_code, bytes_read, body.c_str());
    if (bytes_read > 0)
    {
        rsp = body.substr(0, bytes_read);
        vector<string> lines;
        istringstream iss(rsp.c_str());
        string line;
        while (getline(iss, line))
        {
            lines.push_back(line);
        }
        if (lines.size() > 2)
        {
            string mu = lines[2];
            size_t pos = mu.find(".mp3");
            if (pos != std::string::npos)
            {
                mu = mu.substr(4, pos);
            }
            else
            {
                mu = mu.substr(4);
            }
            return std::string(mu.c_str());
        }
    }
    return "";
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string &music_url)
{
    if (music_url.empty())
    {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }

    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());

    while (true)
    {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto &app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();

        // 等小智把话说完了，变成聆听状态之后，马上转成待机状态，进入音乐播放
        if (current_state == kDeviceStateListening)
        {
            ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            // 切换状态
            app.ToggleChatState(); // 变成待机状态
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
            ;
        }
        else if (current_state != kDeviceStateIdle)
        { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (current_state == kDeviceStateIdle)
        {
            break;
        }
    }

    if (NULL == asp_handle_)
    {
        esp_asp_cfg_t cfg;
        cfg.in.cb = NULL;
        cfg.in.user_ctx = NULL;
        cfg.out.cb = out_data_callback;
        cfg.out.user_ctx = this;
        cfg.task_prio = 5;
        cfg.task_core = 1;
        cfg.task_stack = 16 * 1024;
        // cfg.task_stack_in_ext = false;
        cfg.prev_ctx = NULL;
        cfg.prev = NULL;

        esp_gmf_err_t err = esp_audio_simple_player_new(&cfg, &asp_handle_);
        assert(ESP_OK == err);
        assert(asp_handle_ != NULL);
        err = esp_audio_simple_player_set_event(asp_handle_, mock_event_callback, this);
        assert(ESP_OK == err);
    }
    assert(ESP_OK == esp_audio_simple_player_run(asp_handle_, music_url.c_str(), NULL));
    // 开始播放线程（会等待缓冲区有足够数据）
    ESP_LOGI(TAG, "Streaming threads started successfully");
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming()
{
    if (asp_handle_)
    {
        esp_asp_state_t st = ESP_ASP_STATE_NONE;
        assert(ESP_GMF_ERR_OK == esp_audio_simple_player_get_state(asp_handle_, &st));
        if (st == ESP_ASP_STATE_RUNNING)
        {
            assert(ESP_GMF_ERR_OK == esp_audio_simple_player_stop(asp_handle_));
        }
    }

    auto lcd = Board::GetInstance().GetDisplay();
    if (lcd)
    {
        lcd->SetPlayMusicStatus(false);
    }

    // 重置采样率到原始值
    ResetSampleRate();

    // 清空歌名显示
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        display->SetMusicInfo(""); // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display");
    }

    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

void Esp32Music::AfterPlayDone()
{
    // 播放结束时清空歌名显示
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        display->SetMusicInfo(""); // 清空歌名显示
        ESP_LOGI(TAG, "Cleared song name display on playback end");
    }

    // 重置采样率到原始值
    ResetSampleRate();
    need_to_play_next_ = true;
    std::lock_guard<std::mutex> lock(next_mutex_);
    play_next_cv_.notify_all();
}

void Esp32Music::setBackgroundImage(const std::string &albumId)
{
    std::string url = "https://search.kuwo.cn/r.s?pn=0&rn=1&albumid=" + albumId + "&stype=albuminfo&show_copyright_off=1&alflac=1&pcmp4=1&encoding=utf8&plat=pc&vipver=MUSIC_9.1.1.2_BCS2&devid=38668888&newver=1&pcjson=1";
    std::string response;
    if (!Request(url, response))
    {
        ESP_LOGE(TAG, "Failed to request album cover");
        return;
    }

    cJSON *rsp = cJSON_Parse(response.c_str());
    if (!rsp)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return;
    }

    cJSON *img = cJSON_GetObjectItem(rsp, "img");
    if (!cJSON_IsString(img))
    {
        ESP_LOGE(TAG, "Failed to get album cover URL");
        cJSON_Delete(rsp);
        return;
    }
    std::string cover_url = cJSON_GetStringValue(img);
    cJSON_Delete(rsp);

    if (cover_url.find(".png") != std::string::npos)
    {
        ESP_LOGW(TAG, "Album cover is a PNG image, not processing");
        return;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

    if (!http->Open("GET", cover_url)) {
        throw std::runtime_error("Failed to open URL: " + cover_url);
    }
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
    }

    size_t content_length = http->GetBodyLength();
    char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
    if (data == nullptr) {
        throw std::runtime_error("Failed to allocate memory for image: " + cover_url);
    }
    size_t total_read = 0;
    while (total_read < content_length) {
        int ret = http->Read(data + total_read, content_length - total_read);
        if (ret < 0) {
            heap_caps_free(data);
            throw std::runtime_error("Failed to download image: " + cover_url);
        }
        if (ret == 0) {
            break;
        }
        total_read += ret;
    }
    http->Close();

    auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    display->SetPreviewImage(std::move(image));
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate()
{
    auto &board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 &&
        codec->output_sample_rate() != codec->original_output_sample_rate())
    {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz",
                 codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1))
        { // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        }
        else
        {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 下载歌词
bool Esp32Music::Request(const std::string &url, std::string &response)
{
    ESP_LOGI(TAG, "requesting from: %s", url.c_str());

    // 检查URL是否为空
    if (url.empty())
    {
        ESP_LOGE(TAG, "URL is empty!");
        return false;
    }

    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string current_url = url;
    int redirect_count = 0;
    const int max_redirects = 5; // 最多允许5次重定向

    while (retry_count < max_retries && !success && redirect_count < max_redirects)
    {
        if (retry_count > 0)
        {
            ESP_LOGI(TAG, "Retrying request attempts (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 使用Board提供的HTTP客户端
        
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http)
        {
            ESP_LOGE(TAG, "Failed to create HTTP client for request");
            retry_count++;
            continue;
        }
        // 打开GET连接
        if (!http->Open("GET", current_url))
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection for request");
            retry_count++;
            continue;
        }

        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "HTTP status code: %d", status_code);

        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308)
        {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }

        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300)
        {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }

        // 读取响应
        response.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;

        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read content");
        while (true)
        {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            if (bytes_read > 0)
            {
                buffer[bytes_read] = '\0';
                response += buffer;
                total_read += bytes_read;

                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0)
                {
                    ESP_LOGD(TAG, "requseted %d bytes so far", total_read);
                }
            }
            else if (bytes_read == 0)
            {
                // 正常结束，没有更多数据
                ESP_LOGI(TAG, "[%s] request completed, total bytes: %d", url.c_str(), total_read);
                success = true;
                break;
            }
            else
            {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!response.empty())
                {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, response.length());
                    success = true;
                    break;
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to read data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }

        http->Close();

        if (read_error)
        {
            retry_count++;
            continue;
        }

        // 如果成功读取数据，跳出重试循环
        if (success)
        {
            break;
        }
    }

    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries)
    {
        ESP_LOGE(TAG, "Failed to request after %d attempts", max_retries);
        return false;
    }

    // 记录前几个字节的数据，帮助调试
    if (!response.empty())
    {
        size_t preview_size = std::min(response.size(), size_t(50));
        std::string preview = response.substr(0, preview_size);
        ESP_LOGD(TAG, "request content preview (%d bytes): %s", response.length(), preview.c_str());
    }
    else
    {
        ESP_LOGE(TAG, "Failed to read any data from URL: %s", current_url.c_str());
        return false;
    }

    if (response.length() < 256)
    {
        ESP_LOGE(TAG, "request content is too short, size: %d bytes, content: %s", response.length(), response.c_str());
        return false;
    }
    ESP_LOGI(TAG, "request completed successfully, size: %d bytes", response.length());
    return true;
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string &lyric_content)
{
    ESP_LOGD(TAG, "Parsing lyrics content");

    // 解析响应JSON以提取歌词
    cJSON *rsp = cJSON_Parse(lyric_content.c_str());
    if (!rsp)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    // 获取"data"对象
    cJSON *data = cJSON_GetObjectItem(rsp, "data");
    if (!data || !cJSON_IsObject(data))
    {
        ESP_LOGE(TAG, "Invalid JSON structure - 'data' object not found");
        cJSON_Delete(rsp);
        return false;
    }

    // 获取"lrclist"数组
    cJSON *lrclist = cJSON_GetObjectItem(data, "lrclist");
    if (!lrclist || !cJSON_IsArray(lrclist) || cJSON_GetArraySize(lrclist) == 0)
    {
        ESP_LOGE(TAG, "Cannot get 'lrclist' from JSON!");
        cJSON_Delete(rsp);
        return false;
    }

    // 预先分配内存以减少内存分配开销
    lyrics_.reserve(cJSON_GetArraySize(lrclist));

    for (int i = 0; i < cJSON_GetArraySize(lrclist); i++)
    {
        cJSON *lyric = cJSON_GetArrayItem(lrclist, i);
        cJSON *content = cJSON_GetObjectItem(lyric, "lineLyric");
        cJSON *timeStr = cJSON_GetObjectItem(lyric, "time");

        if (!content || !content->valuestring || !timeStr || !timeStr->valuestring)
        {
            ESP_LOGW(TAG, "Incomplete lyric data at index %d", i);
            continue;
        }

        float time = atof(timeStr->valuestring);
        int time_ms = static_cast<int>(time * 1000);
        std::string content_str = content->valuestring;
        lyrics_.emplace_back(time_ms, content_str);
    }

    if (lyrics_.empty())
    {
        ESP_LOGE(TAG, "Parsed lyrics are empty");
        cJSON_Delete(rsp);
        return false;
    }

    // 排序歌词以确保按时间顺序显示
    std::sort(lyrics_.begin(), lyrics_.end(), [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b)
              { return a.first < b.first; });

    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    cJSON_Delete(rsp);
    return true;
}

bool Esp32Music::ParseRecommondSong(const std::string &keyword, const std::string &songId)
{
    {
        std::lock_guard<std::mutex> lock(next_mutex_);
        song_played_map_.clear();
    }
    std::string url = "https://search.kuwo.cn/r.s?pn=0&rn=10&all=" + url_encode(keyword) + "&ft=playlist&rformat=json&encoding=utf8&pcjson=1";
    std::string rsp1;
    if (!Request(url, rsp1))
    {
        ESP_LOGE(TAG, "Failed to request recommond song, keyword: [%s]", keyword.c_str());
        return false;
    }

    cJSON *rsp = cJSON_Parse(rsp1.c_str());
    if (!rsp)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    cJSON *abslist = cJSON_GetObjectItem(rsp, "abslist");
    if (!cJSON_IsArray(abslist) || cJSON_GetArraySize(abslist) == 0)
    {
        ESP_LOGE(TAG, "Cannot get 'abslist' from JSON!");
        cJSON_Delete(rsp);
        return false;
    }

    int maxSongNum = 0;
    std::string markPlayListId;
    for (int i = 0; i < cJSON_GetArraySize(abslist); i++)
    {
        cJSON *list = cJSON_GetArrayItem(abslist, i);
        cJSON *songnum = cJSON_GetObjectItem(list, "songnum");
        if (!songnum || !songnum->valuestring)
        {
            ESP_LOGE(TAG, "Invalid JSON structure - 'songnum' object not found");
            continue;
        }
        cJSON *playlistid = cJSON_GetObjectItem(list, "playlistid");
        if (!playlistid || !playlistid->valuestring)
        {
            ESP_LOGE(TAG, "Invalid JSON structure - 'playlistid' object not found");
            continue;
        }
        int songNum = atoi(songnum->valuestring);
        if (songNum > maxSongNum)
        {
            maxSongNum = songNum;
            markPlayListId = std::string(playlistid->valuestring);
        }
    }
    cJSON_Delete(rsp);
    if (0 == maxSongNum)
    {
        ESP_LOGE(TAG, "Cannot find recommond song, keyword: [%s]", keyword.c_str());
        return false;
    }

    std::string rsp2;
    url = "http://nplserver.kuwo.cn/pl.svc?op=getlistinfo&pid=" + markPlayListId + "&pn=0&rn=100&encode=utf8&keyset=pl2012&vipver=MUSIC_9.1.1.2_BCS2&newver=1";
    if (!Request(url, rsp2))
    {
        ESP_LOGE(TAG, "Failed to request playlist, keyword: [%s]", keyword.c_str());
        return false;
    }

    rsp = cJSON_Parse(rsp2.c_str());
    if (!rsp)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    cJSON *musiclist = cJSON_GetObjectItem(rsp, "musiclist");
    if (!cJSON_IsArray(musiclist) || cJSON_GetArraySize(musiclist) == 0)
    {
        ESP_LOGE(TAG, "Cannot get 'musiclist' from JSON!");
        cJSON_Delete(rsp);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(next_mutex_);
        for (int i = 0; i < cJSON_GetArraySize(musiclist); i++)
        {
            cJSON *list = cJSON_GetArrayItem(musiclist, i);
            cJSON *musicrId = cJSON_GetObjectItem(list, "id");
            cJSON *albumId = cJSON_GetObjectItem(list, "albumid");
            if (!musicrId || !musicrId->valuestring)
            {
                ESP_LOGE(TAG, "Invalid JSON structure - 'id' object not found");
                continue;
            }
            if (!albumId || !albumId->valuestring)
            {
                ESP_LOGE(TAG, "Invalid JSON structure - 'albumid' object not found");
                continue;
            }
            std::string id = std::string(musicrId->valuestring);
            std::string albumIdStr = std::string(albumId->valuestring);
            song_played_map_[id].first = false;
            song_played_map_[id].second = albumIdStr;
        }
        song_played_map_[songId].first = true;
    }
    cJSON_Delete(rsp);
    return true;
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread()
{
    ESP_LOGI(TAG, "Lyric display thread started");
    std::string lyric_content;
    if (!Request(current_lyric_url_, lyric_content))
    {
        ESP_LOGE(TAG, "Failed to download lrics");
    }
    ParseLyrics(lyric_content);
    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms)
{
    std::lock_guard<std::mutex> lock(lyrics_mutex_);

    if (lyrics_.empty())
    {
        return;
    }

    // 查找当前应该显示的歌词
    int new_lyric_index = -1;

    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;

    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++)
    {
        if (lyrics_[i].first <= current_time_ms)
        {
            new_lyric_index = i;
        }
        else
        {
            break; // 时间戳已超过当前时间
        }
    }

    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1)
    {
        new_lyric_index = -1;
    }

    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_)
    {
        current_lyric_index_ = new_lyric_index;

        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display)
        {
            std::string lyric_text;

            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size())
            {
                lyric_text = lyrics_[current_lyric_index_].second;
            }

            // 显示歌词
            display->SetChatMessage("lyric", lyric_text.c_str());

            ESP_LOGD(TAG, "Lyric update at %lldms: %s",
                     current_time_ms,
                     lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

bool Esp32Music::ClearRecommendedSongs()
{
    std::lock_guard<std::mutex> lock(next_mutex_);
    song_played_map_.clear();
    return true;
}

bool Esp32Music::AddLikedSong()
{
    if (current_song_id_.empty())
    {
        ESP_LOGW(TAG, "No song is playing");
        return false;
    }

    if (song_played_map_.empty())
    {
        return false;
    }

    for (auto &elem : liked_songs_)
    {
        if (elem.first == current_song_id_)
        {
            ESP_LOGW(TAG, "Song [%s] has been liked", current_song_id_.c_str());
            return false;
        }
    }
    std::string album_id = song_played_map_[current_song_id_].second;
    liked_songs_.push_back(std::make_pair(current_song_id_, album_id));
    cJSON *liked_songs_json = cJSON_CreateObject();
    cJSON *songs_array = cJSON_CreateArray();
    for (auto &elem : liked_songs_)
    {
        cJSON *song_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(song_obj, "songId", elem.first.c_str());
        cJSON_AddStringToObject(song_obj, "albumId", elem.second.c_str());
        cJSON_AddItemToArray(songs_array, song_obj);
    }
    cJSON_AddItemToObject(liked_songs_json, "songs", songs_array);
    char *json_str = cJSON_PrintUnformatted(liked_songs_json);
    ESP_LOGW(TAG, "Liked songs: %s", json_str);
    std::string liked_songs_str;
    if (json_str != nullptr)
    {
        liked_songs_str = json_str;
        free(json_str);
    }
    Settings settings("liked_songs", true);
    settings.SetString("songs", liked_songs_str);
    cJSON_Delete(liked_songs_json);
    return true;
}

void Esp32Music::TogglePlayState()
{
    if (asp_handle_)
    {
        esp_asp_state_t st = ESP_ASP_STATE_NONE;
        assert(ESP_GMF_ERR_OK == esp_audio_simple_player_get_state(asp_handle_, &st));
        if (st == ESP_ASP_STATE_RUNNING)
        {
            assert(ESP_GMF_ERR_OK == esp_audio_simple_player_pause(asp_handle_));
        }
        else if (st == ESP_ASP_STATE_PAUSED)
        {
            assert(ESP_GMF_ERR_OK == esp_audio_simple_player_resume(asp_handle_));
        }
    }
}

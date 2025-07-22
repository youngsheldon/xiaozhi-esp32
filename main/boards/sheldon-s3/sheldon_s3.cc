#include "wifi_board.h"
#include "display/lcd_display.h"
#include "audio_codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "power_manager.h"
#include "power_save_timer.h"
#include "assets/lang_config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <esp_sleep.h>
#include "i2c_device.h"
#include <driver/rtc_io.h>

#include "mp3dec.h"
#include "mp3common.h"
#include <opus_resampler.h>

#define TAG "SheldonS3"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class Cst816s : public I2cDevice
{
public:
    struct TouchPoint_t
    {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    Cst816s(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Cst816s()
    {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint()
    {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
        if (tp_.x == 0 || tp_.y == 0 || tp_.x == 4095 || tp_.y == 4095)
        {
            tp_.num = 0;
        }
    }

    const TouchPoint_t &GetTouchPoint()
    {
        return tp_;
    }

private:
    uint8_t *read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

class CustomLcdDisplay : public SpiLcdDisplay
{
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                        {
                            .text_font = &font_puhui_20_4,
                            .icon_font = &font_awesome_20_4,
                            .emoji_font = font_emoji_64_init(),
                        })
    {

        DisplayLockGuard lock(this);
        // 由于屏幕是圆的，所以状态栏需要增加左右内边距
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.33, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.33, 0);
    }
};

class SheldonS3 : public WifiBoard
{
private:
    i2c_master_bus_handle_t i2c_bus_;
    Cst816s *cst816s_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Display *display_;
    PowerSaveTimer *power_save_timer_;
    PowerManager *power_manager_;
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_timer_handle_t touchpad_timer_;
    OpusResampler output_resampler_;

    void InitializeI2c()
    {
        // Initialize I2C peripheral

        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = I2C_SDA_PIN,
            .scl_io_num = I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    static void touchpad_timer_callback(void *arg)
    {
        auto &board = (SheldonS3 &)Board::GetInstance();
        auto touchpad = board.GetTouchpad();
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500; // 触摸时长阈值，超过500ms视为长按

        touchpad->UpdateTouchPoint();
        auto touch_point = touchpad->GetTouchPoint();
        if (touch_point.num > 0)
        {
            ESP_LOGI(TAG, "Touch point: %d, %d", touch_point.x, touch_point.y);
        }

        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched)
        {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        }
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched)
        {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;

            // 只有短触才触发
            if (touch_duration < TOUCH_THRESHOLD_MS)
            {
                auto &app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting &&
                    !WifiStation::GetInstance().IsConnected())
                {
                    board.ResetWifiConfiguration();
                }
                app.ToggleChatState();
            }
        }
    }

    void InitializeCst816sTouchPad()
    {
        ESP_LOGI(TAG, "Init Cst816s");
        cst816s_ = new Cst816s(i2c_bus_, 0x15);

        // 创建定时器，10ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = touchpad_timer_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 10 * 1000)); // 10ms = 10000us
    }

    void InitializePowerManager()
    {
        power_manager_ = new PowerManager(GPIO_NUM_21);
        power_manager_->OnChargingStatusChanged([this](bool is_charging)
                                                {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            } });
    }

    void InitializePowerSaveTimer()
    {
        power_save_timer_ = new PowerSaveTimer(-1, 30, 60);
        power_save_timer_->OnEnterSleepMode([this]()
                                            {
            ESP_LOGI(TAG, "Enabling sleep mode");
            display_->SetChatMessage("system", "");
            display_->SetEmotion("sleepy");
            GetBacklight()->SetBrightness(1); });
        power_save_timer_->OnExitSleepMode([this]()
                                           {
            display_->SetChatMessage("system", "");
            display_->SetEmotion("neutral");
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this]()
                                             {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_pullup_en(GPIO_NUM_1);
            rtc_gpio_pulldown_dis(GPIO_NUM_1);
            esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0);  
            esp_lcd_panel_disp_on_off(panel_handle, false); //关闭显示
            esp_deep_sleep_start(); });
        power_save_timer_->SetEnabled(true);
    }

    // SPI初始化
    void InitializeSpi()
    {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                                              DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01初始化
    void InitializeGc9a01Display()
    {
        ESP_LOGI(TAG, "Init GC9A01 display");

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN; // Set to -1 if not use
        panel_config.rgb_endian = LCD_RGB_ENDIAN_RGB;        // LCD_RGB_ENDIAN_RGB;
        panel_config.bits_per_pixel = 16;                    // Implemented by LCD command `3Ah` (16/18)

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        display_ = new SpiLcdDisplay(io_handle, panel_handle,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_20_4,
                                         .icon_font = &font_awesome_20_4,
                                         .emoji_font = font_emoji_64_init(),
                                     });
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]()
                             {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState(); });

        volume_up_button_.OnClick([this]()
                                  {
                                      power_save_timer_->WakeUp();
                                      auto codec = GetAudioCodec();
                                      auto volume = codec->output_volume() + 10;
                                      if (volume > 100)
                                      {
                                          volume = 100;
                                      }
                                      codec->SetOutputVolume(volume);
                                      GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));

                                      xTaskCreate([](void *arg)
                                                  {
                    SheldonS3* app = (SheldonS3*)arg;
                    app->PlayHttpMp3("http://lw.sycdn.kuwo.cn/49fece36e30fc99bf2a5533cfaf50159/687f3dbe/resource/30106/trackmedia/M500002dOqLZ3Effbw.mp3");
                    vTaskDelete(NULL); },
                                                  "PlayHttpMp3", 4096 * 10, this, 8, NULL); });

        volume_up_button_.OnLongPress([this]()
                                      {
                power_save_timer_->WakeUp();
                GetAudioCodec()->SetOutputVolume(100);
                GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME); });

        volume_down_button_.OnClick([this]()
                                    {
                power_save_timer_->WakeUp();
                auto codec = GetAudioCodec();
                auto volume = codec->output_volume() - 10;
                if (volume < 0) {
                    volume = 0;
                }
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume)); });

        volume_down_button_.OnLongPress([this]()
                                        {
                power_save_timer_->WakeUp();
                GetAudioCodec()->SetOutputVolume(0);
                GetDisplay()->ShowNotification(Lang::Strings::MUTED); });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot()
    {
        auto &thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Battery"));
    }

    bool SkipID3Tag(unsigned char *buffer, int *size)
    {
        if (*size >= 3 && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3')
        {
            if (*size < 10)
                return false;

            int tag_size = ((buffer[6] << 21) | (buffer[7] << 14) | (buffer[8] << 7) | buffer[9]) + 10;
            if (*size >= tag_size)
            {
                memmove(buffer, buffer + tag_size, *size - tag_size);
                *size -= tag_size;
                return true;
            }
            return false;
        }
        return true;
    }

#define MAX_NCHAN 2
#define MAX_NGRAN 2
#define MAX_NSAMP 576
#define INPUT_BUFFER_SIZE (16 * 1024)
#define OUTPUT_BUFFER_SIZE (16 * 1024)

    void PlayHttpMp3(const std::string &url)
    {
        auto codec = Board::GetInstance().GetAudioCodec();
        output_resampler_.Configure(2304, codec->output_sample_rate());
        HMP3Decoder decoder = MP3InitDecoder();
        if (!decoder)
        {
            ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
            return;
        }

        // 使用智能指针管理HTTP对象
        auto http = Board::GetInstance().CreateHttp();
        if (!http)
        {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            MP3FreeDecoder(decoder);
            return;
        }

        // 设置ICY元数据相关头
        http->SetHeader("Icy-MetaData", "1");
        http->SetHeader("Accept-Encoding", "identity;q=1,*;q=0");
        http->SetHeader("Connection", "keep-alive");

        if (!http->Open("GET", url))
        {
            ESP_LOGE(TAG, "Failed to open URL: %s", url.c_str());
            MP3FreeDecoder(decoder);
            return;
        }

        int status_code = http->GetStatusCode();
        if (status_code != 200)
        {
            ESP_LOGE(TAG, "HTTP request failed, status code: %d", status_code);
            MP3FreeDecoder(decoder);
            return;
        }

        // 获取ICY metadata间隔
        int icy_metaint = 0;
        std::string icy_header = http->GetResponseHeader("icy-metaint");
        if (!icy_header.empty())
        {
            icy_metaint = atoi(icy_header.c_str());
            ESP_LOGI(TAG, "ICY metadata interval: %d", icy_metaint);
        }

        unsigned char inputBuffer[INPUT_BUFFER_SIZE];
        int bytesLeft = 0;
        unsigned char *decodePtr = inputBuffer;
        int bytesProcessed = 0; // 用于跟踪ICY元数据

        MP3FrameInfo mp3FrameInfo;
        short outputBuffer[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

        bool is_audio_started = false;

        while (true)
        {
            // 移动剩余数据到buffer开头
            if (decodePtr > inputBuffer && bytesLeft > 0)
            {
                memmove(inputBuffer, decodePtr, bytesLeft);
                decodePtr = inputBuffer;
            }

            // 检查是否需要读取ICY元数据
            if (icy_metaint > 0 && bytesProcessed >= icy_metaint)
            {
                // 读取元数据大小(1字节，实际大小 = 值 * 16)
                unsigned char metaSizeByte = 0;
                int read = http->Read((char *)&metaSizeByte, 1);
                if (read != 1)
                {
                    ESP_LOGE(TAG, "Failed to read metadata size");
                    break;
                }

                int metaDataSize = metaSizeByte * 16;
                bytesProcessed = 0; // 重置处理字节计数

                if (metaDataSize > 0)
                {
                    std::vector<char> metaData(metaDataSize);
                    int totalRead = 0;

                    // 读取完整的元数据
                    while (totalRead < metaDataSize)
                    {
                        int readBytes = http->Read(metaData.data() + totalRead, metaDataSize - totalRead);
                        if (readBytes <= 0)
                        {
                            ESP_LOGE(TAG, "Failed to read complete metadata");
                            break;
                        }
                        totalRead += readBytes;
                    }

                    // 处理元数据 (这里只是简单打印，实际应用中可以解析艺术家/歌曲信息等)
                    if (totalRead == metaDataSize)
                    {
                        std::string metaString(metaData.data(), metaDataSize);
                        ESP_LOGI(TAG, "Metadata: %s", metaString.c_str());
                    }
                }
            }

            // 读取更多数据
            if (bytesLeft < INPUT_BUFFER_SIZE)
            {
                int readBytes = http->Read((char *)(inputBuffer + bytesLeft), INPUT_BUFFER_SIZE - bytesLeft);
                if (readBytes > 0)
                {
                    bytesLeft += readBytes;
                    bytesProcessed += readBytes; // 更新已处理字节数
                }
                else if (readBytes == 0)
                {
                    ESP_LOGI(TAG, "End of stream");
                    break;
                }
                else
                {
                    ESP_LOGE(TAG, "HTTP read error");
                    break;
                }
            }

            // 跳过ID3标签
            if (!SkipID3Tag(decodePtr, &bytesLeft))
            {
                ESP_LOGW(TAG, "ID3 tag too big, increase buffer size");
                continue;
            }

            // 查找同步头
            int offset = MP3FindSyncWord(decodePtr, bytesLeft);
            if (offset == -1)
            {
                ESP_LOGW(TAG, "No sync word found, dropping %d bytes", bytesLeft);
                bytesLeft = 0;
                continue;
            }

            decodePtr += offset;
            bytesLeft -= offset;

            // 解码循环
            while (bytesLeft > 0)
            {
                int samples = MP3Decode(decoder, &decodePtr, &bytesLeft, outputBuffer, 0);

                if (samples == 0)
                {
                    MP3GetLastFrameInfo(decoder, &mp3FrameInfo);

                    if (!is_audio_started)
                    {
                        is_audio_started = true;
                    }

                    ESP_LOGI(TAG, "Frame bitrate: %d, nChans: %d, samprate: %d, bitsPerSample: %d, outputSamps: %d, layer: %d, version: %d",
                             mp3FrameInfo.bitrate, mp3FrameInfo.nChans, mp3FrameInfo.samprate,
                             mp3FrameInfo.bitsPerSample, mp3FrameInfo.outputSamps,
                             mp3FrameInfo.layer, mp3FrameInfo.version);

                    
                    codec->EnableOutput(true);
                    std::vector<int16_t> pcm;
                    pcm.resize(mp3FrameInfo.outputSamps * mp3FrameInfo.nChans);
                    memcpy(pcm.data(), outputBuffer, mp3FrameInfo.outputSamps * mp3FrameInfo.nChans * sizeof(short));
                    // Resample if the sample rate is different
                    if (mp3FrameInfo.outputSamps != codec->output_sample_rate())
                    {
                        int target_size = output_resampler_.GetOutputSamples(pcm.size());
                        std::vector<int16_t> resampled(target_size);
                        output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
                        pcm = std::move(resampled);
                    }
                    codec->OutputData(pcm);
                }
                else if (samples == ERR_MP3_INDATA_UNDERFLOW)
                {
                    break; // 需要更多数据
                }
                else if (samples == ERR_MP3_INVALID_FRAMEHEADER)
                {
                    decodePtr += 1;
                    bytesLeft -= 1;
                }
                else
                {
                    ESP_LOGE(TAG, "MP3 decode error: %d", samples);
                    break;
                }
            }

            // 移动剩余数据到buffer开头
            if (bytesLeft > 0 && decodePtr > inputBuffer)
            {
                memmove(inputBuffer, decodePtr, bytesLeft);
                decodePtr = inputBuffer;
            }
        }

        // 确保资源释放
        MP3FreeDecoder(decoder);
        http->Close();
    }

public:
    SheldonS3() : boot_button_(BOOT_BUTTON_GPIO), volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                  volume_down_button_(VOLUME_DOWN_BUTTON_GPIO)
    {
        // InitializeI2c();
        // InitializeCst816sTouchPad();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        InitializeIot();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led *GetLed() override
    {
        static SingleLed led_strip(BUILTIN_LED_GPIO);
        return &led_strip;
    }

    virtual Display *GetDisplay() override
    {
        return display_;
    }

    virtual Backlight *GetBacklight() override
    {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_RIGHT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_LEFT); // I2S_STD_SLOT_LEFT / I2S_STD_SLOT_RIGHT / I2S_STD_SLOT_BOTH
        return &audio_codec;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override
    {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override
    {
        if (!enabled)
        {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }

    Cst816s *GetTouchpad()
    {
        return cst816s_;
    }
};

DECLARE_BOARD(SheldonS3);

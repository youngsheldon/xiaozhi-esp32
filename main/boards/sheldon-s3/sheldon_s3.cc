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

// 双缓冲区结构定义
#define BUFFER_COUNT 2
#define BUFFER_SIZE (4096 * 8) // 增大缓冲区至32KB

typedef struct
{
    uint8_t data[BUFFER_SIZE];
    size_t size;
    bool ready;
    SemaphoreHandle_t mutex;
} AudioBuffer;

AudioBuffer audioBuffers[BUFFER_COUNT];
QueueHandle_t bufferQueue; // 缓冲区队列，用于通知解码任务
bool decodingActive = false;

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
                    app->PlayHttpMp3("http://er.sycdn.kuwo.cn/6f0fcc6ad951672c7ab7acdb23a1d608/687e4426/resource/30106/trackmedia/M5000039MnYb0qxYhV.mp3");
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

    // 优化的MP3解码函数（支持双缓冲区和错误恢复）
    static void mp3DecodingTask(void *pvParameters)
    {
        HMP3Decoder decoder = MP3InitDecoder();
        if (decoder == NULL)
        {
            ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
            vTaskDelete(NULL);
            return;
        }

        MP3FrameInfo frame_info;
        int16_t outBuf[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
        AudioBuffer *buffer;

        decodingActive = true;
        while (decodingActive)
        {
            // 等待可用的缓冲区
            if (xQueueReceive(bufferQueue, &buffer, portMAX_DELAY) != pdTRUE)
            {
                continue;
            }

            xSemaphoreTake(buffer->mutex, portMAX_DELAY);
            if (buffer->size == 0)
            {
                xSemaphoreGive(buffer->mutex);
                continue;
            }

            uint8_t *readPtr = buffer->data;
            int bytesAvailable = buffer->size;
            int offset = 0;
            int decodeErrors = 0;

            // 循环解码当前缓冲区中的所有MP3帧
            while (bytesAvailable > 4)
            { // 至少保留4字节用于同步字检测
                offset = MP3FindSyncWord(readPtr, bytesAvailable);
                if (offset < 0)
                {
                    break; // 没有找到同步字，退出循环
                }

                readPtr += offset;
                bytesAvailable -= offset;

                // 尝试解码帧
                int err = MP3Decode(decoder, &readPtr, &bytesAvailable, outBuf, 0);
                if (err)
                {
                    ESP_LOGE(TAG, "MP3 decode error: %d", err);

                    // 错误恢复逻辑
                    if (err == ERR_MP3_MAINDATA_UNDERFLOW)
                    {
                        // 主数据不足，尝试跳过部分数据
                        if (bytesAvailable > 1024)
                        {
                            readPtr += 512;
                            bytesAvailable -= 512;
                        }
                        decodeErrors++;

                        // 如果连续错误超过阈值，退出当前缓冲区处理
                        if (decodeErrors > 5)
                        {
                            break;
                        }
                        continue;
                    }
                    else
                    {
                        // 其他致命错误，退出解码
                        break;
                    }
                }
                else
                {
                    decodeErrors = 0; // 重置错误计数器
                    MP3GetLastFrameInfo(decoder, &frame_info);
                    ESP_LOGI(TAG, "Frame bitrate: %d, nChans: %d, samprate: %d, bitsPerSample: %d, outputSamps: %d, layer: %d, version: %d", frame_info.bitrate, frame_info.nChans, frame_info.samprate, frame_info.bitsPerSample, frame_info.outputSamps, frame_info.layer, frame_info.version);
                    // 处理解码后的PCM数据
                    // configureTX(frame_info.samprate, ...);
                    // write((uint8_t *)outBuf, ...);
                }
            }

            // 标记缓冲区为可用状态
            buffer->size = 0;
            buffer->ready = false;
            xSemaphoreGive(buffer->mutex);
        }

        MP3FreeDecoder(decoder);
        vTaskDelete(NULL);
    }

    // 优化的HTTP MP3流播放函数
    void PlayHttpMp3(const std::string &url)
    {
        ESP_LOGI(TAG, "Start playing MP3 from URL: %s", url.c_str());
        // 初始化双缓冲区
        for (int i = 0; i < BUFFER_COUNT; i++)
        {
            audioBuffers[i].size = 0;
            audioBuffers[i].ready = false;
            audioBuffers[i].mutex = xSemaphoreCreateMutex();
        }

        bufferQueue = xQueueCreate(BUFFER_COUNT, sizeof(AudioBuffer *));
        xTaskCreate(mp3DecodingTask, "mp3_decoder", 8192, NULL, 5, NULL);

        auto http = Board::GetInstance().CreateHttp();
        http->SetHeader("Icy-MetaData", "1");
        http->SetHeader("Accept-Encoding", "identity;q=1,*;q=0");
        http->SetHeader("Connection", "keep-alive");

        if (!http->Open("GET", url))
        {
            ESP_LOGE(TAG, "Failed to open URL: %s", url.c_str());
            decodingActive = false;
            return;
        }

        auto status_code = http->GetStatusCode();
        if (status_code != 200)
        {
            ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
            http->Close();
            decodingActive = false;
            return;
        }

        int currentBuffer = 0;
        int bytesRead = 0;
        bool eofReached = false;

        // 主循环：持续读取数据到缓冲区
        while (!eofReached && decodingActive)
        {
            // 获取当前可用缓冲区
            xSemaphoreTake(audioBuffers[currentBuffer].mutex, portMAX_DELAY);

            // 只有当缓冲区为空时才填充数据
            if (audioBuffers[currentBuffer].size == 0)
            {
                int len = http->Read((char*)audioBuffers[currentBuffer].data, BUFFER_SIZE);

                if (len <= 0)
                {
                    ESP_LOGI(TAG, "End of stream or read error");
                    eofReached = true;
                }
                else
                {
                    audioBuffers[currentBuffer].size = len;
                    audioBuffers[currentBuffer].ready = true;

                    // 将缓冲区指针放入队列通知解码任务
                    xQueueSend(bufferQueue, &audioBuffers[currentBuffer], portMAX_DELAY);
                }
            }

            xSemaphoreGive(audioBuffers[currentBuffer].mutex);

            // 切换到下一个缓冲区
            currentBuffer = (currentBuffer + 1) % BUFFER_COUNT;

            // 短暂延时，避免CPU占用过高
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // 等待所有缓冲区处理完毕
        vTaskDelay(pdMS_TO_TICKS(500));
        decodingActive = false;
        http->Close();

        // 清理资源
        for (int i = 0; i < BUFFER_COUNT; i++)
        {
            vSemaphoreDelete(audioBuffers[i].mutex);
        }
        vQueueDelete(bufferQueue);
    }

    // bool playMP3(uint8_t *src, size_t src_len)
    // {
    //     int16_t outBuf[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
    //     uint8_t *readPtr = NULL;
    //     int bytesAvailable = 0, err = 0, offset = 0;
    //     MP3FrameInfo frame_info;
    //     HMP3Decoder decoder = NULL;

    //     bytesAvailable = src_len;
    //     readPtr = src;

    //     decoder = MP3InitDecoder();
    //     if (decoder == NULL)
    //     {
    //         ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
    //         return false;
    //     }

    //     do
    //     {
    //         offset = MP3FindSyncWord(readPtr, bytesAvailable);
    //         if (offset < 0)
    //         {
    //             break;
    //         }
    //         readPtr += offset;
    //         bytesAvailable -= offset;
    //         err = MP3Decode(decoder, &readPtr, &bytesAvailable, outBuf, 0);
    //         if (err)
    //         {
    //             ESP_LOGE(TAG, "MP3 decode error: %d", err);
    //             MP3FreeDecoder(decoder);
    //             return false;
    //         }
    //         else
    //         {
    //             MP3GetLastFrameInfo(decoder, &frame_info);
    //             ESP_LOGI(TAG, "Frame bitrate: %d, nChans: %d, samprate: %d, bitsPerSample: %d, outputSamps: %d, layer: %d, version: %d", frame_info.bitrate, frame_info.nChans, frame_info.samprate, frame_info.bitsPerSample, frame_info.outputSamps, frame_info.layer, frame_info.version);
    //             // configureTX(frameInfo.samprate, (i2s_data_bit_width_t)frameInfo.bitsPerSample, (i2s_slot_mode_t)frameInfo.nChans);
    //             // write((uint8_t *)outBuf, (size_t)((frameInfo.bitsPerSample / 8) * frameInfo.outputSamps));

    //         }
    //     } while (true);
    //     MP3FreeDecoder(decoder);
    //     return true;
    // }

    // void PlayHttpMp3(const std::string &url)
    // {
    //     auto http = Board::GetInstance().CreateHttp();
    //     http->SetHeader("Icy-MetaData", "1");
    //     http->SetHeader("Accept-Encoding", "identity;q=1,*;q=0");
    //     http->SetHeader("Connection", "keep-alive");

    //     if (!http->Open("GET", url))
    //     {
    //         ESP_LOGE(TAG, "Failed to open URL: %s", url.c_str());
    //         return;
    //     }

    //     auto status_code = http->GetStatusCode();
    //     if (status_code != 200)
    //     {
    //         ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
    //         return;
    //     }

    //     int bufferSize = 4096*5; // 4KB buffer
    //     char buffer[bufferSize];
    //     int size = http->GetBodyLength();
    //     int bytesRead = 0;
    //     while ((bytesRead < size))
    //     {
    //         int len = http->Read(buffer, bufferSize);
    //         if (len <= 0)
    //         {
    //             ESP_LOGE(TAG, "Failed to read data from URL: %s", url.c_str());
    //             break;
    //         }

    //         playMP3((uint8_t *)buffer, len);

    //         bytesRead += len;
    //     }
    //     http->Close();
    // }

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

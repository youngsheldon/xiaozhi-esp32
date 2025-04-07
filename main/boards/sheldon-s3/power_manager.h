#pragma once
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>

class PowerManager
{
private:
    esp_timer_handle_t timer_handle_;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    std::vector<uint16_t> adc_values_;
    uint32_t battery_level_ = 0;
    uint32_t old_average_adc = 0;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    int ticks_ = 0;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 20;

    adc_oneshot_unit_handle_t adc_handle_;

    void CheckBatteryStatus()
    {
        // 如果电池电量数据不足，则读取电池电量数据
        if (adc_values_.size() < kBatteryAdcDataCount)
        {
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据充足，则每 kBatteryAdcInterval 个 tick 读取一次电池电量数据
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0)
        {
            ReadBatteryAdcData();
        }
    }

    void ReadBatteryAdcData()
    {
        int adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, ADC_CHANNEL_1, &adc_value));

        // 将 ADC 值添加到队列中
        adc_values_.push_back(adc_value);
        if (adc_values_.size() > kBatteryAdcDataCount)
        {
            adc_values_.erase(adc_values_.begin());
        }
        uint32_t average_adc = 0;
        for (auto value : adc_values_)
        {
            average_adc += value;
        }
        average_adc /= adc_values_.size();

        // 定义电池电量区间
        const struct
        {
            uint16_t adc;
            uint8_t level;
        } levels[] = {
            {1770, 0},
            {1962, 20},
            {2154, 40},
            {2346, 60},
            {2538, 80},
            {2770, 100}};

        // 低于最低值时
        if (average_adc < levels[0].adc)
        {
            battery_level_ = 0;
        }
        // 高于最高值时
        else if (average_adc >= levels[5].adc)
        {
            battery_level_ = 100;
        }
        else
        {
            // 线性插值计算中间值
            for (int i = 0; i < 5; i++)
            {
                if (average_adc >= levels[i].adc && average_adc < levels[i + 1].adc)
                {
                    float ratio = static_cast<float>(average_adc - levels[i].adc) / (levels[i + 1].adc - levels[i].adc);
                    battery_level_ = levels[i].level + ratio * (levels[i + 1].level - levels[i].level);
                    break;
                }
            }
        }

        // Check low battery status
        if (adc_values_.size() >= kBatteryAdcDataCount)
        {
            bool new_low_battery_status = battery_level_ <= kLowBatteryLevel;
            if (new_low_battery_status != is_low_battery_)
            {
                is_low_battery_ = new_low_battery_status;
                if (on_low_battery_status_changed_)
                {
                    on_low_battery_status_changed_(is_low_battery_);
                }
            }
        }
        if(average_adc > old_average_adc)
        {
            is_charging_ = true;
        }
        else
        {
            is_charging_ = false;
        }

        ESP_LOGI("PowerManager", "ADC value: %d average: %ld level: %ld", adc_value, average_adc, battery_level_);
        old_average_adc = average_adc;
    }

public:
    PowerManager()
    {
        // 创建电池电量检查定时器
        esp_timer_create_args_t timer_args = {
            .callback = [](void *arg)
            {
                PowerManager *self = static_cast<PowerManager *>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));

        // 初始化 ADC
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_1, &chan_config));
    }

    ~PowerManager()
    {
        if (timer_handle_)
        {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_)
        {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging()
    {
        // 如果电量已经满了，则不再显示充电中
        if (battery_level_ == 100)
        {
            return false;
        }
        return is_charging_;
    }

    bool IsDischarging()
    {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_;
    }

    uint8_t GetBatteryLevel()
    {
        return battery_level_;
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback)
    {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback)
    {
        on_charging_status_changed_ = callback;
    }
};

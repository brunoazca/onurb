#include <cstring>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "pmu.h"

static const char *TAG = "onurb_pmu";

static XPowersPMU s_pmu;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static int pmu_read(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 1000) == ESP_OK ? 0 : -1;
}

static int pmu_write(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    uint8_t buf[16];

    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 1000) == ESP_OK ? 0 : -1;
}

extern "C" esp_err_t onurb_pmu_init(void)
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = AXP2101_SLAVE_ADDRESS;
    dev_cfg.scl_speed_hz = 400000;

    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_pmu.begin(AXP2101_SLAVE_ADDRESS, pmu_read, pmu_write)) {
        ESP_LOGE(TAG, "AXP2101 not responding");
        return ESP_FAIL;
    }

    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.enableTemperatureMeasure();

    /* Board has no battery thermistor; Waveshare: leaving TS detection on
     * "will cause abnormal charging". */
    s_pmu.disableTSPinMeasure();

    s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
#if CONFIG_ONURB_CHARGE_CURRENT_400MA
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    const int charge_ma = 400;
#else
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);
    const int charge_ma = 200;
#endif
    s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    s_ready = true;
    ESP_LOGI(TAG, "charger configured: %d mA CC, 4.2 V cut-off, TS detection off", charge_ma);
    return ESP_OK;
}

extern "C" void onurb_pmu_log_status(void)
{
    if (!s_ready) {
        return;
    }

    static const char *const states[] = {"trickle", "pre-charge", "constant current",
                                         "constant voltage", "done", "not charging"};
    uint8_t st = s_pmu.getChargerStatus();

    ESP_LOGI(TAG, "B-06: battery %s, %u mV, %d%% | USB %s | %s (%s) | AXP2101 %.1f C",
             s_pmu.isBatteryConnect() ? "connected" : "ABSENT",
             (unsigned)s_pmu.getBattVoltage(),
             s_pmu.getBatteryPercent(),
             s_pmu.isVbusIn() ? "in" : "out",
             s_pmu.isCharging() ? "charging" : "not charging",
             st < sizeof(states) / sizeof(states[0]) ? states[st] : "?",
             s_pmu.getTemperature());
}

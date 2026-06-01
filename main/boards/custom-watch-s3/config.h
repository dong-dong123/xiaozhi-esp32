#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ========== 扬声器 I2S (NS4168 功放, I2S_NUM_0 TX) ==========
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

#define SPEAKER_I2S_GPIO_BCLK GPIO_NUM_12
#define SPEAKER_I2S_GPIO_LRC  GPIO_NUM_13
#define SPEAKER_I2S_GPIO_DOUT GPIO_NUM_11

// ========== 麦克风 I2S (MS4030, I2S_NUM_1 RX) ==========
#define MIC_I2S_GPIO_BCLK GPIO_NUM_40
#define MIC_I2S_GPIO_WS   GPIO_NUM_41
#define MIC_I2S_GPIO_DIN  GPIO_NUM_39

// ========== CO5300 QSPI AMOLED 1.96" 410x502 ==========
#define DISPLAY_WIDTH   410
#define DISPLAY_HEIGHT  502
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false
#define DISPLAY_INVERT_COLOR false
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

// QSPI 信号
#define DISPLAY_QSPI_CS    GPIO_NUM_3
#define DISPLAY_QSPI_CLK   GPIO_NUM_8
#define DISPLAY_QSPI_SIO0  GPIO_NUM_15
#define DISPLAY_QSPI_SIO1  GPIO_NUM_16
#define DISPLAY_QSPI_SIO2  GPIO_NUM_17
#define DISPLAY_QSPI_SIO3  GPIO_NUM_18
#define DISPLAY_QSPI_RST   GPIO_NUM_9
#define DISPLAY_QSPI_TE    GPIO_NUM_10

// ========== 触摸 I2C_NUM_0 (CST82x, 共享 SCL=4/SDA=5) ==========
#define TOUCH_I2C_SCL  GPIO_NUM_4
#define TOUCH_I2C_SDA  GPIO_NUM_5
#define TOUCH_RST      GPIO_NUM_7
#define TOUCH_INT      GPIO_NUM_6

// ========== I2C_NUM_0 设备地址 (SCL=GPIO_4, SDA=GPIO_5) ==========
// 0x15: CST82x 触摸
// 0x13: BMM150 罗盘
// 0x6B: QMI8658 六轴陀螺仪
// 0x51: RTC 时钟

// ========== 六轴陀螺仪 QMI8658 ==========
#define QMI8658_INT1   GPIO_NUM_21

// ========== RTC 时钟 ==========
#define RTC_INT        GPIO_NUM_42

// ========== 充电与电源管理 ==========
#define CHECK_CHARGE_PIN       GPIO_NUM_1
#define ONE_CLICK_STARTUP_PIN  GPIO_NUM_2
#define BAT_CHECK_ADC_PIN      GPIO_NUM_14

// ========== 按键 ==========
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_47
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_48

// ========== USB ==========
#define USB_DN_GPIO  GPIO_NUM_19
#define USB_DP_GPIO  GPIO_NUM_20

#endif // _BOARD_CONFIG_H_

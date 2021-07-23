# PX4 BootLoader适配GD32的尝试
实验对象：手头的不知名GD32F405开发板；
初期目的：点灯~
## 修改BL硬件配置
主要使用MCU型号相近的OMNIBUSF4SD目标进行开发；
```
omnibusf4sd_bl: $(MAKEFILE_LIST) $(LIBOPENCM3)
	${MAKE} ${MKFLAGS} -f  Makefile.f4 TARGET_HW=OMNIBUSF4SD LINKER_FILE=stm32f4.ld TARGET_FILE_NAME=$@
```
对应的硬件配置位于hw_config.h中，对应宏为TARGET_HW_OMNIBUSF4SD
```
#elif  defined(TARGET_HW_OMNIBUSF4SD)

# define APP_LOAD_ADDRESS               0x08008000
# define BOOTLOADER_DELAY               5000
# define INTERFACE_USB                  1
# define INTERFACE_USART                0
# define USBDEVICESTRING                "PX4 OmnibusF4SD"
# define USBPRODUCTID                   0x0016

# define BOARD_TYPE                     42
# define BOARD_FLASH_SECTORS            11
# define BOARD_FLASH_SIZE               (1024 * 1024)
# define BOARD_FIRST_FLASH_SECTOR_TO_ERASE    1
# define APP_RESERVATION_SIZE           (1 * 16 * 1024) /* 1 16 Kib Sectors */

# define OSC_FREQ                       8

# define BOARD_PIN_LED_ACTIVITY         GPIO5
# define BOARD_PIN_LED_BOOTLOADER       GPIO4
# define BOARD_PORT_LEDS                GPIOB
# define BOARD_CLOCK_LEDS               RCC_AHB1ENR_IOPBEN
# define BOARD_LED_ON                   gpio_clear
# define BOARD_LED_OFF                  gpio_set

# define BOARD_USB_VBUS_SENSE_DISABLED

# define USBMFGSTRING                   "Vertile"
```

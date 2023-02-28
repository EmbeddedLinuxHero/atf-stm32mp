#!/bin/bash

make CROSS_COMPILE=arm-ostl-linux-gnueabi- PLAT=stm32mp1 ARCH=aarch32 ARM_ARCH_MAJOR=7 AARCH32_SP=sp_min DTB_FILE_NAME=stm32mp151a-kiwi.dtb STM32MP_SDMMC=1 STM32MP_EMMC=1 STM32MP_USB_PROGRAMMER=1
cp build/stm32mp1/release/tf-a-stm32mp151a-kiwi.stm32 tf-a_usb.stm32
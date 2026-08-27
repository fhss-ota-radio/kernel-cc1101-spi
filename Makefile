# SPDX-License-Identifier: GPL-2.0
# out-of-tree 커널 모듈 빌드 (라즈베리파이 타깃)
#
# 사용법:
#   네이티브 빌드 (라즈베리파이 위에서 직접):
#     make
#   크로스 컴파일 (예: 64비트 RPi OS, 커널 소스 트리 별도 준비):
#     make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#          KDIR=/path/to/linux
#   설치 / 정리:
#     sudo make modules_install && sudo depmod -a
#     make clean

MODULE_NAME := cc1101_FHSS

obj-m := $(MODULE_NAME).o
cc1101_FHSS-objs := cc1101_core.o cc1101_main.o cc1101_fhss.o cc1101_hop.o

KVERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod $(MODULE_NAME).ko

unload:
	sudo rmmod $(MODULE_NAME)

.PHONY: all modules modules_install clean load unload

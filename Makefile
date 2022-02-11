PWD := $(shell pwd)
obj-m := Read_mem_sector.o
KERNEL := /home/invlko-016/Desktop/buildroot-2021.02.9/output/build/linux-5.10.7
CROSS := /home/invlko-016/Desktop/buildroot-2021.02.9/output/host/bin/aarch64-buildroot-linux-uclibc-
all :
        make -C $(KERNEL) ARCH=arm64 CROSS_COMPILE=$(CROSS) M=$(PWD) modules
clean:
        make -C lib/modules/$(shell uname -r)/build M=$(PWD) clean


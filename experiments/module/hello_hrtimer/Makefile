KDIR := /tmp/kbuild

obj-m += hello_hrtimer.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
KDIR := /tmp/kbuild

KBUILD_EXTRA_SYMBOLS := /work/experiments/module/my_add/Module.symvers

obj-m += hello.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
KDIR := /tmp/kbuild

obj-m += hello_dt.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
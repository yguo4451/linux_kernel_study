KDIR := /tmp/kbuild

obj-m += my_add.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
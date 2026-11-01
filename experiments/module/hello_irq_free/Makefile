KDIR := /tmp/kbuild

obj-m += hello_irq_free.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean

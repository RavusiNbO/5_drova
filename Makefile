obj-m += edu_dma.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(CC) -Wall -Wextra -O2 -o test test.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) test

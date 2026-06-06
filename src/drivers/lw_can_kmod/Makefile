# Out-of-tree build for the lw_can SocketCAN USB driver.
#
# Kbuild places build artifacts next to the sources it compiles. To keep this
# directory clean, we build *inside* build/: the sources are symlinked in and a
# Kbuild is generated there, so every artifact (.o, .ko, .cmd, Module.symvers,
# modules.order, ...) is confined to build/.
#
#   make           ->  build/lw_can.ko
#   make clean     ->  removes build/
#   sudo modprobe can-dev && sudo insmod build/lw_can.ko

MODULE    := lw_can
KDIR      ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR := build
SRCS      := $(wildcard *.c *.h)

all: $(BUILD_DIR)/Kbuild
	$(MAKE) -C $(KDIR) M=$(CURDIR)/$(BUILD_DIR) modules

# Stage sources (as symlinks) and a generated Kbuild into build/.
$(BUILD_DIR)/Kbuild: $(SRCS) Makefile
	@mkdir -p $(BUILD_DIR)
	@for f in $(SRCS); do ln -sf ../$$f $(BUILD_DIR)/$$f; done
	@printf 'obj-m += $(MODULE).o\n$(MODULE)-y := main.o\n' > $@

clean:
	$(RM) -r $(BUILD_DIR)

.PHONY: all clean

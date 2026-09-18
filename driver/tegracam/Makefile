# Standalone cross-build of nv_imx296.ko against JetPack 6.2 OOT headers.
#
# Unlike NVIDIA in-tree drivers/media/i2c/Makefile this does not set -Werror:
# the driver originated on JP7.2 and we do not want cross-generation warnings
# turned fatal.

KDIR  ?= /lib/modules/$(shell uname -r)/build
NVOOT ?= /usr/src/nvidia/nvidia-oot

obj-m += nv_imx296.o

# -I$(src)            -> resolves #include <nvidia/conftest.h> (our JP6.2 shim)
# -I$(NVOOT)/include  -> media/tegracam_core.h, media/camera_common.h, ...
ccflags-y += -I$(src) -I$(NVOOT)/include -Wno-address -Wno-enum-int-mismatch

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
	    KBUILD_EXTRA_SYMBOLS=$(NVOOT)/Module.symvers modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean

include common.mk
# Adds waslr runtime to the sdk 

WASLR_RT_DIR := $(SDK_ROOT)/share/waslr-rt
# non target specific directory
WASLR_INCL_DIR := $(SYSROOT)/include/waslr

.PHONY: install clean

all: install

install:
	@echo "Installing WASLR Runtime..."
	mkdir -p $(WASLR_RT_DIR)
	cp $(SUPPORT_ROOT)/waslr.c $(WASLR_RT_DIR)/
	cp $(SUPPORT_ROOT)/waslr.h $(WASLR_RT_DIR)/
	mkdir -p $(WASLR_INCL_DIR)
	ln -rsf $(WASLR_RT_DIR)/waslr.h $(WASLR_INCL_DIR)/waslr.h

clean:
	rm -rf $(WASLR_RT_DIR) $(WASLR_INCL_DIR)
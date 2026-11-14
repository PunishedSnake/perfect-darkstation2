# pdsnd.irx: Perfect Dark SPU2 hardware-voice module (docs/ps2-spu2-audio.md).
# Built with the PS2SDK IOP toolchain:  make -C port/ps2/iop/pdsnd
IOP_BIN = pdsnd.irx
IOP_OBJS = pdsnd.o imports.o
IOP_CFLAGS += -I../.. -Wall -Werror
IOP_OBJS_DIR ?= obj/

all: $(IOP_BIN)

clean:
	rm -rf $(IOP_OBJS_DIR) $(IOP_BIN) *.elf

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal

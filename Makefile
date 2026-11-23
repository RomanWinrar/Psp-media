TARGET = MediaPlayer
OBJS = main.o

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

FFMPEG_PKGS = libavformat libavcodec libswscale libswresample libavutil
FFMPEG_CFLAGS := $(shell psp-pkg-config --cflags $(FFMPEG_PKGS) 2>/dev/null)
FFMPEG_LIBS   := $(shell psp-pkg-config --libs $(FFMPEG_PKGS) 2>/dev/null)
ifneq ($(strip $(FFMPEG_LIBS)),)
CFLAGS += -DHAVE_FFMPEG $(FFMPEG_CFLAGS)
endif

LIBS = $(FFMPEG_LIBS) -lmad -ljpeg -lpng -lz -lm -lpspaudio -lpsppower -lpspdebug -lpspdisplay -lpspge -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Media Player

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

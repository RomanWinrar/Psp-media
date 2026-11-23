TARGET = MediaPlayer
OBJS = main.o

BUILD_PRX = 1
PSP_LARGE_MEMORY = 1

CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

PSPDEV_DIR := $(shell psp-config --pspdev-path 2>/dev/null)
FFMPEG_PKGS = libavformat libavcodec libswscale libswresample libavutil
FFMPEG_CFLAGS := $(shell PKG_CONFIG_PATH="$(PSPDEV_DIR)/psp/lib/pkgconfig" pkg-config --cflags $(FFMPEG_PKGS) 2>/dev/null)
FFMPEG_LIBS   := $(shell PKG_CONFIG_PATH="$(PSPDEV_DIR)/psp/lib/pkgconfig" pkg-config --libs $(FFMPEG_PKGS) 2>/dev/null)

ifeq ($(strip $(FFMPEG_LIBS)),)
ifneq ($(wildcard $(PSPDEV_DIR)/psp/include/libavformat/avformat.h),)
FFMPEG_CFLAGS = -I$(PSPDEV_DIR)/psp/include
FFMPEG_LIBS   = -lavformat -lavcodec -lswscale -lswresample -lavutil
endif
endif

ifneq ($(strip $(FFMPEG_LIBS)),)
CFLAGS += -DHAVE_FFMPEG $(FFMPEG_CFLAGS)
endif

LIBS = $(FFMPEG_LIBS) -lmad -ljpeg -lpng -lz -lm -lpspaudio -lpsppower -lpspdebug -lpspdisplay -lpspge -lpspctrl

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Media Player

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

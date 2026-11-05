.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := c7s-loader
BUILD       := build
SOURCES     := source vendor/imgui
DATA        := data
INCLUDES    := include vendor/imgui
APP_TITLE   := C7S-Loader
APP_AUTHOR  := Herbrax
APP_VERSION := 0.1.0

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections -std=gnu11 \
               -D_GNU_SOURCE $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__ -DIMGUI_USER_CONFIG=\"imgui_config.h\"
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-rtti
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS        := -lmicrohttpd -lnx -lpthread -lm
LIBDIRS     := $(PORTLIBS) $(LIBNX) $(CURDIR)/.deps/switch

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(sort $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))) fonts_data.c)
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD   := $(CC)
else
export LD   := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all deps dist

WEB_ASSETS := $(CURDIR)/include/web_assets.h
MHD_LIB    := $(CURDIR)/.deps/switch/lib/libmicrohttpd.a
FONTS_DATA := $(CURDIR)/source/fonts_data.c

all: $(BUILD)

deps: $(MHD_LIB) $(WEB_ASSETS) $(FONTS_DATA)

$(WEB_ASSETS): $(wildcard $(CURDIR)/web/*) $(CURDIR)/tools/gen_assets.py
	python3 $(CURDIR)/tools/gen_assets.py

$(MHD_LIB):
	DEVKITPRO=$(DEVKITPRO) bash $(CURDIR)/tools/build-switch-deps.sh

$(FONTS_DATA): $(wildcard $(CURDIR)/assets/fonts/*.ttf) $(CURDIR)/tools/gen_fonts.py
	python3 $(CURDIR)/tools/gen_fonts.py

$(BUILD): deps
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

dist: $(BUILD)
	@mkdir -p dist/switch/C7S-Loader
	@cp $(TARGET).nro dist/switch/
	@cp payloads/*.C7S dist/switch/C7S-Loader/
	@echo "SD layout ready in dist/switch"

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro : $(OUTPUT).elf
endif

$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

-include $(DEPENDS)

endif

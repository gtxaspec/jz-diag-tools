# Cross compiler prefix
CROSS_COMPILE ?= mipsel-linux-

# Force CC to always use CROSS_COMPILE prefix
CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -fPIC -std=gnu99 -Os -ffunction-sections -fdata-sections -fomit-frame-pointer
LDFLAGS ?= -Wl,--gc-sections

# Additional flags that should always be included
EXTRA_CFLAGS = -ldl -lm -pthread
EXTRA_LDFLAGS = 

# Source file
SRC = jz_gpio.c

# Output binary name
OUT = ingenic-gpio

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(SRC) $(LDFLAGS) $(EXTRA_LDFLAGS) -o $(OUT)

clean:
	rm -f $(OUT)

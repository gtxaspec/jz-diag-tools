/*
    This file is part of jz-diag-tools.
    Copyright (C) 2022 Reimu NotMoe <reimu@sudomaker.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <ctype.h>

uint32_t read_soc_id(); // Function prototype for read_soc_id

static void show_help() {
	puts(
		"Usage: ingenic-gpio <show|[GPIO_DEF [COMMAND VALUE]]>\n"
		"GPIO diagnostic tool for Ingenic Tomahawk Series SoCs.\n"
		"\n"
		"Commands:\n"
		"  inl                        Read input level\n"
		"  int                        Set interrupt\n"
		"  msk                        Set mask\n"
		"  pat0                       Set pattern 0 (data)\n"
		"  pat1                       Set pattern 1 (direction)\n"
		"  gpio_input                 Shortcut of `int 0', `msk 1', `pat1 1'\n"
		"  gpio_output                Shortcut of `int 0', `msk 1', `pat1 0'\n"
		"  read                       Shortcut of `inl'\n"
		"  write                      Shortcut of `pat0'\n"
		"  func                       Shortcut of `int 0', `msk 0', `pat1 <1>', `pat0 <0>'\n"
		"  drive                      Set drive strength (0-3 for 2ma, 4ma, 8ma, 12ma)\n"
		"\n"
		"Examples:\n"
		"  ingenic-gpio show\n"
		"  ingenic-gpio pc23 gpio_input\n"
		"  ingenic-gpio pc23 read\n"
		"  ingenic-gpio pa00 gpio_output\n"
		"  ingenic-gpio pa00 write 1\n"
		"  ingenic-gpio pb27 func 0  # Set PB27 as 24MHz clock output on X1000\n"
	);

}

#define GPIO_BASE		0x10010000
#define CONTROL_REG     0x1300002C // Control register address
#define PAGE_SIZE 4096  // Define a constant for the page size

#define PxDRVL_OFFSET 0x130
#define PxDRVH_OFFSET 0x140

#define BIT_GET(x, n)		(((x) >> (n)) & 1)
#define BIT_SET(x, n)		((x) |= (1 << (n)))
#define BIT_CLR(x, n)		((x) &= ~(1 << (n)))

typedef struct {
	volatile const uint32_t INL;
	volatile const uint32_t _rsvd0[3];
	volatile uint32_t INT;
	volatile uint32_t INTS;
	volatile uint32_t INTC;
	volatile const uint32_t _rsvd1[1];
	volatile uint32_t MSK;
	volatile uint32_t MSKS;
	volatile uint32_t MSKC;
	volatile const uint32_t _rsvd2[1];
	volatile uint32_t PAT1;
	volatile uint32_t PAT1S;
	volatile uint32_t PAT1C;
	volatile const uint32_t _rsvd3[1];
	volatile uint32_t PAT0;
	volatile uint32_t PAT0S;
	volatile uint32_t PAT0C;
	volatile const uint32_t _rsvd4[1];
	volatile const uint32_t FLG;
	volatile const uint32_t _rsvd5[1];
	volatile const uint32_t FLGC;
	volatile const uint32_t _rsvd6[5];
	volatile uint32_t PEN;
	volatile uint32_t PENS;
	volatile uint32_t PENC;
	volatile const uint32_t _rsvd7[29];
	volatile uint32_t GID2LD;
} XHAL_GPIO_HandleTypeDef;

static void *phys_mem = NULL;

static uint8_t get_drive_strength(volatile XHAL_GPIO_HandleTypeDef *port, uint8_t offset);
static uint8_t drive_strength_to_ma(uint8_t strength);
uint32_t GPIO_PORT_WIDTH = 0x100; // Default value

static void show_gpios() {
	for (int i = 0; i < 3; i++) {
		volatile XHAL_GPIO_HandleTypeDef *port = phys_mem + i * GPIO_PORT_WIDTH;

		printf("Port %c\n", 'A' + i);
		printf("================\n");

		for (int j = 0; j < 32; j++) {
			printf("P%c%02u [%02d]: ", 'A' + i, j, i * 32 + j);
			uint8_t drive_strength = get_drive_strength(port, j);

			bool b_int = BIT_GET(port->INT, j);
			bool b_msk = BIT_GET(port->MSK, j);
			bool b_pat1 = BIT_GET(port->PAT1, j);
			bool b_pat0 = BIT_GET(port->PAT0, j);
			bool b_inl = BIT_GET(port->INL, j); // Get the input level

			if (b_int) {
				if (b_pat1) {
					if (b_pat0) {
						printf("INTERRUPT RISING_EDGE ");
					} else {
						printf("INTERRUPT FALLING_EDGE ");
					}
				} else {
					if (b_pat0) {
						printf("INTERRUPT HIGH_LEVEL ");
					} else {
						printf("INTERRUPT LOW_LEVEL ");
					}
				}

				if (b_msk) {
					printf("DISABLED ");
				} else {
					printf("ENABLED ");
				}
			} else {
				if (b_msk) {
					printf(b_pat1 ? "GPIO INPUT %u " : "GPIO OUTPUT %u ", b_pat0 ? b_inl : b_pat0);
				} else {
					printf("FUNCTION %d ", b_pat1 << 1 | b_pat0);
				}
			}
			printf("%dma\n", drive_strength_to_ma(drive_strength));
		}

		printf("\n");
	}
}

static bool str2portoff(const char *str, void **port, uint8_t *offset) {
	if (strlen(str) != 4) {
		return false;
	}

	uint8_t portchar = toupper(str[1]);
	if (portchar < 'A' || portchar > 'G') {
		return false;
	}

	uint8_t off = strtol(str+2, NULL, 10);
	if (off > 31) {
		return false;
	}

	*port = phys_mem + (portchar - 'A') * GPIO_PORT_WIDTH;
	*offset = off;

	return true;
}

static void gpio_read_inl(void *port_addr, uint8_t offset) {
	volatile XHAL_GPIO_HandleTypeDef *port = port_addr;

	printf("%u\n", BIT_GET(port->INL, offset));
}

static long check_val(const char *val) {
	if (!val) {
		printf("error: value not specified");
		exit(2);
	}

	return strtol(val, NULL, 10);
}

uint32_t read_soc_id() {
    int fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) {
        perror("Error opening /dev/mem");
        return 0;
    }

    void *map_base = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_SHARED, fd, CONTROL_REG & ~(PAGE_SIZE - 1));
    if (map_base == MAP_FAILED) {
        close(fd);
        perror("Error mapping memory");
        return 0;
    }

    volatile uint32_t *reg = (volatile uint32_t *)(map_base + (CONTROL_REG & (PAGE_SIZE - 1)));
    uint32_t soc_id = *reg;

    munmap(map_base, PAGE_SIZE);
    close(fd);
    return soc_id;
}

void set_port_width() {
    uint32_t soc_id = read_soc_id();
    uint32_t soc_type;

    // Extracting the relevant bits from soc_id to determine the SOC type
    if ((soc_id >> 28) != 1) {
        // For SOC types where upper 4 bits are 1, use bits 12-19
        soc_type = (soc_id >> 12) & 0xFF;
    } else {
        // For other SOC types (like T10/T20), use a different method
        soc_type = ((soc_id << 4) >>
            0x10);
    }

    // Set GPIO_PORT_WIDTH based on soc_type
    switch (soc_type) {
	    case 5: // Assuming this is for T10
            GPIO_PORT_WIDTH = 0x100;
            break;
        case 0x2000: // Assuming this is for T20
            GPIO_PORT_WIDTH = 0x100;
            break;
        case 0x21:
        case 0x30:
        case 0x31:
            GPIO_PORT_WIDTH = 0x1000;
            break;
        default:
            GPIO_PORT_WIDTH = 0x100; // Default value
            break;
    }
    // Debug
    // printf("SOC ID: 0x%08X\n", soc_id);
    // printf("SOC Type: 0x%04X\n", soc_type);
}

static uint8_t drive_strength_to_ma(uint8_t strength) {
    switch (strength) {
        case 0: return 2;
        case 1: return 4;
        case 2: return 8;
        case 3: return 12;
        default: return 0; // Invalid strength
    }
}

static uint8_t get_drive_strength(volatile XHAL_GPIO_HandleTypeDef *port, uint8_t offset) {
    uint32_t *drive_reg;
    uint32_t mask = 3 << (offset * 2); // Each pin has 2 bits for drive strength
    uint8_t strength;

    if (offset < 16) {
        // For lower pins (0-15)
        drive_reg = (uint32_t *)((uint8_t *)port + PxDRVL_OFFSET);
    } else {
        // For higher pins (16 and above)
        drive_reg = (uint32_t *)((uint8_t *)port + PxDRVH_OFFSET);
        offset -= 16; // Adjust offset for high pins
    }

    strength = (*drive_reg & mask) >> (offset * 2);
    return strength;
}

static void set_drive_strength(volatile XHAL_GPIO_HandleTypeDef *port, uint8_t offset, uint8_t strength) {
    if (strength > 3) {
        printf("Invalid drive strength. Must be 0-3.\n");
        return;
    }

    uint32_t *drive_reg;
    uint32_t mask = 3 << (offset * 2); // Each pin has 2 bits for drive strength

    if (offset < 16) {
        // For lower pins (0-15)
        drive_reg = (uint32_t *)((uint8_t *)port + PxDRVL_OFFSET);
    } else {
        // For higher pins (16 and above)
        drive_reg = (uint32_t *)((uint8_t *)port + PxDRVH_OFFSET);
        offset -= 16;  // Adjust offset for high pins
    }

    *drive_reg &= ~mask; // Clear existing strength bits
    *drive_reg |= (strength << (offset * 2)); // Set new strength
}

int main(int argc, char **argv) {
	set_port_width();

	if (argc < 2) {
		show_help();
		return 1;
	}

	int fd = open("/dev/mem", O_RDWR|O_SYNC);

	if (fd < 0) {
		perror("error: failed to open /dev/mem");
		return 2;
	}

	phys_mem = mmap(NULL, 0x10000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);

	if (phys_mem == MAP_FAILED) {
		perror("error: mmap failed");
		return 2;
	}

	if (0 == strcmp(argv[1], "show")) {
		show_gpios();
	} else {
		volatile XHAL_GPIO_HandleTypeDef *port;
		uint8_t offset;
		const char *val = argv[3];

		if (str2portoff(argv[1], (void **) &port, &offset)) {
			if (!argv[2]) {
				printf("error: no command specified\n");
				return 2;
			}

			if (0 == strcmp(argv[2], "inl") || 0 == strcmp(argv[2], "read")) {
				printf("%u\n", BIT_GET(port->INL, offset));
			} else if (0 == strcmp(argv[2], "int")) {
				uint8_t v = check_val(val);
				if (v) {
					BIT_SET(port->INTS, offset);
				} else {
					BIT_SET(port->INTC, offset);
				}
			} else if (0 == strcmp(argv[2], "pat0") || 0 == strcmp(argv[2], "write")) {
				uint8_t v = check_val(val);
				if (v) {
					BIT_SET(port->PAT0S, offset);
				} else {
					BIT_SET(port->PAT0C, offset);
				}
			} else if (0 == strcmp(argv[2], "flip")) {
				printf("Flipping... Ctrl-C to exit.\n");
				while (1) {
					BIT_SET(port->PAT0S, offset);
					BIT_SET(port->PAT0C, offset);
				}
			} else if (0 == strcmp(argv[2], "pat1")) {
				uint8_t v = check_val(val);
				if (v) {
					BIT_SET(port->PAT1S, offset);
				} else {
					BIT_SET(port->PAT1C, offset);
				}
			} else if (0 == strcmp(argv[2], "gpio_input")) {
				BIT_SET(port->INTC, offset);
				BIT_SET(port->MSKS, offset);
				BIT_SET(port->PAT1S, offset);
			} else if (0 == strcmp(argv[2], "gpio_output")) {
				BIT_SET(port->INTC, offset);
				BIT_SET(port->MSKS, offset);
				BIT_SET(port->PAT1C, offset);
			} else if (0 == strcmp(argv[2], "func")) {
				uint8_t v = check_val(val);

				BIT_SET(port->INTC, offset);
				BIT_SET(port->MSKC, offset);

				BIT_GET(v, 1) ? BIT_SET(port->PAT1S, offset) : BIT_SET(port->PAT1C, offset);
				BIT_GET(v, 0) ? BIT_SET(port->PAT0S, offset) : BIT_SET(port->PAT0C, offset);
			} else if (0 == strcmp(argv[2], "drive")) {
				if (!argv[3]) {
					printf("error: drive strength value not specified\n");
					return 2;
				}
				uint8_t drive_strength = (uint8_t)strtol(argv[3], NULL, 10);
				set_drive_strength(port, offset, drive_strength);
			} else {
				printf("error: Bad command `%s'\n", argv[2]);
			}
		} else {
			printf("error: Bad pin specification `%s'\n", argv[1]);
			return 2;
		}
	}

	return 0;
}

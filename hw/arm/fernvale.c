/*
 * Fernvale emulation
 *
 * Copyright (c) 2014 Sean Cross
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/devices.h"
#include "hw/boards.h"
#include "hw/char/serial.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "hw/loader.h"
#include "sysemu/blockdev.h"
#include "hw/block/flash.h"
#include "exec/address-spaces.h"

#include "sysemu/char.h"

#define FERNVALE_DEBUG_UART "/dev/ttymxc2"
#define FERNVALE_DEBUG_PROMPT "fernly>"
//static FILE *fernvale_fp;
static int fernvale_fd;

#define FV_IRAM_SIZE 0xd000
#define FV_IRAM_BASE 0x70000000
#define FV_UART_BASE 0xa0080000

#define FV_PSRAM_SIZE 8 * 1024 * 1024
#define FV_PSRAM_BASE 0

#define TYPE_FERNVALE_UART "fernvale-uart"
#define FERNVALE_UART(obj) \
        OBJECT_CHECK(FernvaleUARTState, (obj), TYPE_FERNVALE_UART)

static void fernvale_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;

    cpu_reset(CPU(cpu));

    /* Place the PC at the newly-copied offset */
    env->regs[15] = 0x70006600;

    /* R4 contains... something. */
    env->regs[4] = 0x70002040;
    env->regs[5] = 0x70008714;
    env->regs[6] = 0x700086f8;
    env->regs[7] = 0x70006860;
}

static int my_readline(int fd, char *buf, int len)
{
    int offset = 0;
    ssize_t ret;
    uint8_t byte;

    while (len) {

        ret = read(fd, &byte, 1);

        if (-1 == ret)
            return ret;

        if (byte == '\r') // Ignore \r
            continue;

        if (byte == '\n') {
            buf[offset++] = '\0';
            return offset;
        }

        else
            buf[offset++] = byte;
    }

    buf[offset++] = '\0';
    return offset;
}

static uint64_t fernvale_generic_mem_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint32_t base = (uint32_t)opaque;
    uint32_t offset = base + addr;
    char cmd[128];
    uint32_t ret;
    int len;

    /* Write command out */
    switch(size) {
        case 1:
            len = snprintf(cmd, sizeof(cmd)-1, "ro%08x\r\n", offset);
            break;
        case 2:
            len = snprintf(cmd, sizeof(cmd)-1, "rt%08x\r\n", offset);
            break;
        case 4:
        default:
            len = snprintf(cmd, sizeof(cmd)-1, "rf%08x\r\n", offset);
            break;
    }
    ret = write(fernvale_fd, cmd, len);
    if (-1 == ret) {
        perror("Unable to write to serial port");
        return -1;
    }

    /* Read the line back */
    len = my_readline(fernvale_fd, cmd, sizeof(cmd));
    if (len == -1) {
        perror("Unable to read line");
        return -1;
    }

    /* Read result back */
    len = my_readline(fernvale_fd, cmd, sizeof(cmd));
    if (len == -1) {
        perror("Unable to read result");
        return -1;
    }

    ret = strtoul(cmd + 10, NULL, 16);

    /* Read prompt */
    len = my_readline(fernvale_fd, cmd, sizeof(cmd));
    if (-1 == len) {
        perror("Unable to read prompt");
        return -1;
    }

    if (strncmp(cmd, FERNVALE_DEBUG_PROMPT, strlen(FERNVALE_DEBUG_PROMPT))) {
        printf("Sync error!  Expected [%s], got [%s]\n", FERNVALE_DEBUG_PROMPT, cmd);
    }

    printf("READ Fernvale 0x%08x = 0x%08x\n", offset, ret);
    return ret;
}

static void fernvale_generic_mem_write(void *opaque, hwaddr addr,
                                 uint64_t val, unsigned size)
{
    uint32_t base = (uint32_t)opaque;
    uint32_t offset = base + addr;
    char cmd[128];
    int ret;
    int len;
    uint32_t value = val;

    /* Write command out */
    switch(size) {
        case 1:
            len = snprintf(cmd, sizeof(cmd)-1, "wo%08x %02x\r\n", offset, 0xff & value);
            printf("WRITE Fernvale 0x%08x = 0x%02x\n", offset, 0xff & (value));
            break;
        case 2:
            len = snprintf(cmd, sizeof(cmd)-1, "wt%08x %04x\r\n", offset, 0xffff & value);
            printf("WRITE Fernvale 0x%08x = 0x%04x\n", offset, 0xffff & value);
            break;
        case 4:
        default:
            len = snprintf(cmd, sizeof(cmd)-1, "wf%08x %08x\r\n", offset, value);
            printf("WRITE Fernvale 0x%08x = 0x%08x\n", offset, value);
            break;
    }
    ret = write(fernvale_fd, cmd, len);
    if (-1 == ret) {
        perror("Unable to write to serial port");
        return;
    }

    /* Read the line back */
    len = my_readline(fernvale_fd, cmd, sizeof(cmd));
    if (len == -1) {
        perror("Unable to read line");
        return;
    }

    /* Read result back */
    len = my_readline(fernvale_fd, cmd, sizeof(cmd));
    if (len == -1) {
        perror("Unable to read result");
        return;
    }

    /* Read prompt */
    len = my_readline(fernvale_fd, cmd, sizeof(cmd));
    if (-1 == len) {
        perror("Unable to read prompt");
        return;
    }

    if (strncmp(cmd, FERNVALE_DEBUG_PROMPT, strlen(FERNVALE_DEBUG_PROMPT))) {
        printf("Sync error!  Expected [%s], got [%s]\n", FERNVALE_DEBUG_PROMPT, cmd);
    }
}

static const MemoryRegionOps fernvale_generic_mem_ops = {
    .read = fernvale_generic_mem_read,
    .write = fernvale_generic_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void fernvale_hook_memory(uint32_t base, const char *name)
{
    MemoryRegion *hook = g_new(MemoryRegion, 1);
    MemoryRegion *address_space = get_system_memory();

    memory_region_init_io(hook, NULL, &fernvale_generic_mem_ops,
            (void *)base, name, 0x10000);
    memory_region_add_subregion(address_space, base, hook);
}

static void fernvale_init(QEMUMachineInitArgs *args)
{
    const char *cpu_model = args->cpu_model;
    ARMCPU *cpu;
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *iram = g_new(MemoryRegion, 1);
    MemoryRegion *psram = g_new(MemoryRegion, 1);
    DriveInfo *dinfo;
    int flash_size;

    fernvale_fd = open(FERNVALE_DEBUG_UART, O_RDWR);
    if (-1 == fernvale_fd) {
        perror("Unable to open debug uart " FERNVALE_DEBUG_UART);
        exit(1);
    }
    /*
    fernvale_fp = fopen(FERNVALE_DEBUG_UART, "rw");
    if (fernvale_fp == NULL) {
        perror("Unable to open debug uart " FERNVALE_DEBUG_UART);
        exit(1);
    }
    setlinebuf(fernvale_fp);
    */

    if (!cpu_model) {
        cpu_model = "arm926";
    }
    cpu = cpu_arm_init(cpu_model);
    if (!cpu) {
        fprintf(stderr, "Unable to find CPU definition\n");
        exit(1);
    }

    memory_region_init_ram(iram, NULL, "fernvale.iram", FV_IRAM_SIZE);
    vmstate_register_ram_global(iram);
    memory_region_add_subregion(address_space_mem, FV_IRAM_BASE, iram);

    memory_region_init_ram(psram, NULL, "fernvale.psram", FV_PSRAM_SIZE);
    vmstate_register_ram_global(psram);
    memory_region_add_subregion(address_space_mem, FV_PSRAM_BASE, psram);

    /* Register SPI NOR flash */
    dinfo = drive_get(IF_PFLASH, 0, 0);
    if (!dinfo) {
        fprintf(stderr, "No flash image specified.  "
                "Specify with -pflash [spi ROM]\n");
        exit(1);
    }

    flash_size = bdrv_getlength(dinfo->bdrv);
    if (flash_size != 8*1024*1024) {
        fprintf(stderr, "Invalid flash image size (expected 8MB)\n");
        exit(1);
    }

    pflash_cfi02_register(0x10000000, NULL,
                          "fernvale.spi", flash_size,
                          dinfo->bdrv, 0x10000,
                          (flash_size + 0xffff) >> 16,
                          1,
                          2, 0x00BF, 0x236D, 0x0000, 0x0000,
                          0x5555, 0x2AAA, 0);

    /* Hack to read bootloader */
    /* For stage 1, load 8468 bytes from (0x800+444) to address 0x70006600:
Boot header:
Signature: BRLY
Unknown 1: 0x0054
Unknown 2: 0x0000
Unknown 3: 0x00000001
Start: 0x00000800
End: 0x0000b704

Signature: BBBB
Unknown 1: 0x0007
Unknown 2 (important): 0x0002
Start: 0x00003400
End: 0x0000b704
Flags: 0x00000000

Id: FILE_INFO
Version: 1
Type: 1
Flash device: 7
File size: 8468
Max size: -1
Signature type: 1
Signature length: 32
Load address: 0x70006600
Content offset: 444
Jump offset: 444
Attributes: 1
*/

    uint8_t bootsect_block[16384];
    bdrv_read(dinfo->bdrv, 0, bootsect_block, sizeof(bootsect_block)/512);
    rom_add_blob_fixed("intbl", &bootsect_block[0x800 + 444], 8468, 0x70006600);


    /* Add serial port */
    {
        DeviceState *dev = qdev_create(NULL, TYPE_FERNVALE_UART);
        qdev_prop_set_chr(dev, "chardev", serial_hds[0]);
        qdev_init_nofail(dev);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, FV_UART_BASE);
    }

    fernvale_hook_memory(0xa0700000, "unknown");
    fernvale_hook_memory(0xa0030000, "power");

    qemu_register_reset(fernvale_cpu_reset, cpu);
}

/* UART Ports */
#define UART_RBR 0x00
#define UART_THR 0x00
#define UART_IER 0x04
#define UART_IIR 0x08
#define UART_FCR 0x08
#define UART_LCR 0x0c
#define UART_MCR 0x10
#define UART_LSR 0x14
#define UART_MSR 0x18
#define UART_SCR 0x1c

#define UART_SPEED 0x24

/* The following are active when LCR[7] = 1 */
#define UART_DLL 0x100
#define UART_DLH 0x104

/* The following are active when LCR = 0xbf */
#define UART_EFR   0x208
#define UART_XON1  0x210
#define UART_XON2  0x214
#define UART_XOFF1 0x218
#define UART_XOFF2 0x21c

#define UTCR0 0x00
#define UTCR1 0x04
#define UTCR2 0x08
#define UTCR3 0x0c
#define UTDR  0x14
#define UTSR0 0x1c
#define UTSR1 0x20

#define UTCR0_PE  (1 << 0) /* Parity enable */
#define UTCR0_OES (1 << 1) /* Even parity */
#define UTCR0_SBS (1 << 2) /* 2 stop bits */
#define UTCR0_DSS (1 << 3) /* 8-bit data */

#define UTCR3_RXE (1 << 0) /* Rx enable */
#define UTCR3_TXE (1 << 1) /* Tx enable */
#define UTCR3_BRK (1 << 2) /* Force Break */
#define UTCR3_RIE (1 << 3) /* Rx int enable */
#define UTCR3_TIE (1 << 4) /* Tx int enable */
#define UTCR3_LBM (1 << 5) /* Loopback */

#define UTSR0_TFS (1 << 0) /* Tx FIFO nearly empty */
#define UTSR0_RFS (1 << 1) /* Rx FIFO nearly full */
#define UTSR0_RID (1 << 2) /* Receiver Idle */
#define UTSR0_RBB (1 << 3) /* Receiver begin break */
#define UTSR0_REB (1 << 4) /* Receiver end break */
#define UTSR0_EIF (1 << 5) /* Error in FIFO */

#define UTSR1_RNE (1 << 1) /* Receive FIFO not empty */
#define UTSR1_TNF (1 << 2) /* Transmit FIFO not full */
#define UTSR1_PRE (1 << 3) /* Parity error */
#define UTSR1_FRE (1 << 4) /* Frame error */
#define UTSR1_ROR (1 << 5) /* Receive Over Run */

#define RX_FIFO_PRE (1 << 8)
#define RX_FIFO_FRE (1 << 9)
#define RX_FIFO_ROR (1 << 10)

typedef struct FernvaleUARTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharDriverState *chr;
    qemu_irq irq;

    uint8_t utcr0;
    uint16_t brd;
    uint8_t utcr3;
    uint8_t utsr0;
    uint8_t utsr1;

    uint8_t tx_fifo[8];
    uint8_t tx_start;
    uint8_t tx_len;
    uint16_t rx_fifo[12]; /* value + error flags in high bits */
    uint8_t rx_start;
    uint8_t rx_len;

    uint64_t char_transmit_time; /* time to transmit a char in ticks*/
    bool wait_break_end;
    QEMUTimer *rx_timeout_timer;
    QEMUTimer *tx_timer;
} FernvaleUARTState;

static void fernvale_uart_update_status(FernvaleUARTState *s)
{
    uint16_t utsr1 = 0;

    if (s->tx_len != 8) {
        utsr1 |= UTSR1_TNF;
    }

    if (s->rx_len != 0) {
        uint16_t ent = s->rx_fifo[s->rx_start];

        utsr1 |= UTSR1_RNE;
        if (ent & RX_FIFO_PRE) {
            s->utsr1 |= UTSR1_PRE;
        }
        if (ent & RX_FIFO_FRE) {
            s->utsr1 |= UTSR1_FRE;
        }
        if (ent & RX_FIFO_ROR) {
            s->utsr1 |= UTSR1_ROR;
        }
    }

    s->utsr1 = utsr1;
}

static void fernvale_uart_update_int_status(FernvaleUARTState *s)
{
    uint16_t utsr0 = s->utsr0 &
            (UTSR0_REB | UTSR0_RBB | UTSR0_RID);
    int i;

    if ((s->utcr3 & UTCR3_TXE) &&
                (s->utcr3 & UTCR3_TIE) &&
                s->tx_len <= 4) {
        utsr0 |= UTSR0_TFS;
    }

    if ((s->utcr3 & UTCR3_RXE) &&
                (s->utcr3 & UTCR3_RIE) &&
                s->rx_len > 4) {
        utsr0 |= UTSR0_RFS;
    }

    for (i = 0; i < s->rx_len && i < 4; i++)
        if (s->rx_fifo[(s->rx_start + i) % 12] & ~0xff) {
            utsr0 |= UTSR0_EIF;
            break;
        }

    s->utsr0 = utsr0;
    qemu_set_irq(s->irq, utsr0);
}

#if 0
static void fernvale_uart_update_parameters(FernvaleUARTState *s)
{
    int speed, parity, data_bits, stop_bits, frame_size;
    QEMUSerialSetParams ssp;

    /* Start bit. */
    frame_size = 1;
    if (s->utcr0 & UTCR0_PE) {
        /* Parity bit. */
        frame_size++;
        if (s->utcr0 & UTCR0_OES) {
            parity = 'E';
        } else {
            parity = 'O';
        }
    } else {
            parity = 'N';
    }
    if (s->utcr0 & UTCR0_SBS) {
        stop_bits = 2;
    } else {
        stop_bits = 1;
    }

    data_bits = (s->utcr0 & UTCR0_DSS) ? 8 : 7;
    frame_size += data_bits + stop_bits;
    speed = 3686400 / 16 / (s->brd + 1);
    ssp.speed = speed;
    ssp.parity = parity;
    ssp.data_bits = data_bits;
    ssp.stop_bits = stop_bits;
    s->char_transmit_time =  (get_ticks_per_sec() / speed) * frame_size;
    if (s->chr) {
        qemu_chr_fe_ioctl(s->chr, CHR_IOCTL_SERIAL_SET_PARAMS, &ssp);
    }

    DPRINTF(stderr, "%s speed=%d parity=%c data=%d stop=%d\n", s->chr->label,
            speed, parity, data_bits, stop_bits);
}
#endif

static void fernvale_uart_rx_to(void *opaque)
{
    FernvaleUARTState *s = opaque;

    if (s->rx_len) {
        s->utsr0 |= UTSR0_RID;
        fernvale_uart_update_int_status(s);
    }
}

static void fernvale_uart_rx_push(FernvaleUARTState *s, uint16_t c)
{
    if ((s->utcr3 & UTCR3_RXE) == 0) {
        /* rx disabled */
        return;
    }

    if (s->wait_break_end) {
        s->utsr0 |= UTSR0_REB;
        s->wait_break_end = false;
    }

    if (s->rx_len < 12) {
        s->rx_fifo[(s->rx_start + s->rx_len) % 12] = c;
        s->rx_len++;
    } else
        s->rx_fifo[(s->rx_start + 11) % 12] |= RX_FIFO_ROR;
}

static int fernvale_uart_can_receive(void *opaque)
{
    FernvaleUARTState *s = opaque;

    if (s->rx_len == 12) {
        return 0;
    }
    /* It's best not to get more than 2/3 of RX FIFO, so advertise that much */
    if (s->rx_len < 8) {
        return 8 - s->rx_len;
    }
    return 1;
}

static void fernvale_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    FernvaleUARTState *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        fernvale_uart_rx_push(s, buf[i]);
    }

    /* call the timeout receive callback in 3 char transmit time */
    timer_mod(s->rx_timeout_timer,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->char_transmit_time * 3);

    fernvale_uart_update_status(s);
    fernvale_uart_update_int_status(s);
}

static void fernvale_uart_event(void *opaque, int event)
{
    FernvaleUARTState *s = opaque;
    if (event == CHR_EVENT_BREAK) {
        s->utsr0 |= UTSR0_RBB;
        fernvale_uart_rx_push(s, RX_FIFO_FRE);
        s->wait_break_end = true;
        fernvale_uart_update_status(s);
        fernvale_uart_update_int_status(s);
    }
}

static void fernvale_uart_tx(void *opaque)
{
    FernvaleUARTState *s = opaque;
    uint64_t new_xmit_ts = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->utcr3 & UTCR3_LBM) /* loopback */ {
        fernvale_uart_receive(s, &s->tx_fifo[s->tx_start], 1);
    } else if (s->chr) {
        qemu_chr_fe_write(s->chr, &s->tx_fifo[s->tx_start], 1);
    }

    s->tx_start = (s->tx_start + 1) % 8;
    s->tx_len--;
    if (s->tx_len) {
        timer_mod(s->tx_timer, new_xmit_ts + s->char_transmit_time);
    }
    fernvale_uart_update_status(s);
    fernvale_uart_update_int_status(s);
}

static uint64_t fernvale_uart_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    //FernvaleUARTState *s = opaque;
    uint16_t ret;

    printf("%s: Reading from UART (addr 0x%08llx)\n", __func__, addr);
    ret = 0;
    return ret;
#if 0
    switch (addr) {
    case UTCR0:
        return s->utcr0;

    case UTCR1:
        return s->brd >> 8;

    case UTCR2:
        return s->brd & 0xff;

    case UTCR3:
        return s->utcr3;

    case UTDR:
        if (s->rx_len != 0) {
            ret = s->rx_fifo[s->rx_start];
            s->rx_start = (s->rx_start + 1) % 12;
            s->rx_len--;
            fernvale_uart_update_status(s);
            fernvale_uart_update_int_status(s);
            return ret;
        }
        return 0;

    case UTSR0:
        return s->utsr0;

    case UTSR1:
        return s->utsr1;

    default:
        printf("%s: Bad register 0x" TARGET_FMT_plx "\n", __func__, addr);
        return 0;
    }
#endif
}

static void fernvale_uart_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    //FernvaleUARTState *s = opaque;

    printf("%s: Writing to UART (addr 0x%08llx -> 0x%llx)\n", __func__, addr, value);
#if 0
    switch (addr) {
    case UTCR0:
        s->utcr0 = value & 0x7f;
        //fernvale_uart_update_parameters(s);
        break;

    case UTCR1:
        s->brd = (s->brd & 0xff) | ((value & 0xf) << 8);
        //fernvale_uart_update_parameters(s);
        break;

    case UTCR2:
        s->brd = (s->brd & 0xf00) | (value & 0xff);
        //fernvale_uart_update_parameters(s);
        break;

    case UTCR3:
        s->utcr3 = value & 0x3f;
        if ((s->utcr3 & UTCR3_RXE) == 0) {
            s->rx_len = 0;
        }
        if ((s->utcr3 & UTCR3_TXE) == 0) {
            s->tx_len = 0;
        }
        fernvale_uart_update_status(s);
        fernvale_uart_update_int_status(s);
        break;

    case UTDR:
        if ((s->utcr3 & UTCR3_TXE) && s->tx_len != 8) {
            s->tx_fifo[(s->tx_start + s->tx_len) % 8] = value;
            s->tx_len++;
            fernvale_uart_update_status(s);
            fernvale_uart_update_int_status(s);
            if (s->tx_len == 1) {
                fernvale_uart_tx(s);
            }
        }
        break;

    case UTSR0:
        s->utsr0 = s->utsr0 & ~(value &
                (UTSR0_REB | UTSR0_RBB | UTSR0_RID));
        fernvale_uart_update_int_status(s);
        break;

    default:
        printf("%s: Bad register 0x" TARGET_FMT_plx "\n", __func__, addr);
    }
#endif
}

static const MemoryRegionOps fernvale_uart_ops = {
    .read = fernvale_uart_read,
    .write = fernvale_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int fernvale_uart_init(SysBusDevice *dev)
{
    FernvaleUARTState *s = FERNVALE_UART(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &fernvale_uart_ops, s,
                          "uart", 0x10000);
    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);

    s->rx_timeout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fernvale_uart_rx_to, s);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fernvale_uart_tx, s);

    if (s->chr) {
        qemu_chr_add_handlers(s->chr,
                        fernvale_uart_can_receive,
                        fernvale_uart_receive,
                        fernvale_uart_event,
                        s);
    }

    return 0;
}

static void fernvale_uart_reset(DeviceState *dev)
{
    FernvaleUARTState *s = FERNVALE_UART(dev);

    s->utcr0 = UTCR0_DSS; /* 8 data, no parity */
    s->brd = 23;    /* 9600 */
    /* enable send & recv - this actually violates spec */
    s->utcr3 = UTCR3_TXE | UTCR3_RXE;

    s->rx_len = s->tx_len = 0;

    //fernvale_uart_update_parameters(s);
    fernvale_uart_update_status(s);
    fernvale_uart_update_int_status(s);
}

static int fernvale_uart_post_load(void *opaque, int version_id)
{
    FernvaleUARTState *s = opaque;

    //fernvale_uart_update_parameters(s);
    fernvale_uart_update_status(s);
    fernvale_uart_update_int_status(s);

    /* tx and restart timer */
    if (s->tx_len) {
        fernvale_uart_tx(s);
    }

    /* restart rx timeout timer */
    if (s->rx_len) {
        timer_mod(s->rx_timeout_timer,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->char_transmit_time * 3);
    }

    return 0;
}

static const VMStateDescription vmstate_fernvale_uart_regs = {
    .name = "fernvale-uart",
    .version_id = 0,
    .minimum_version_id = 0,
    .minimum_version_id_old = 0,
    .post_load = fernvale_uart_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(utcr0, FernvaleUARTState),
        VMSTATE_UINT16(brd, FernvaleUARTState),
        VMSTATE_UINT8(utcr3, FernvaleUARTState),
        VMSTATE_UINT8(utsr0, FernvaleUARTState),
        VMSTATE_UINT8_ARRAY(tx_fifo, FernvaleUARTState, 8),
        VMSTATE_UINT8(tx_start, FernvaleUARTState),
        VMSTATE_UINT8(tx_len, FernvaleUARTState),
        VMSTATE_UINT16_ARRAY(rx_fifo, FernvaleUARTState, 12),
        VMSTATE_UINT8(rx_start, FernvaleUARTState),
        VMSTATE_UINT8(rx_len, FernvaleUARTState),
        VMSTATE_BOOL(wait_break_end, FernvaleUARTState),
        VMSTATE_END_OF_LIST(),
    },
};

static Property fernvale_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", FernvaleUARTState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void fernvale_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = fernvale_uart_init;
    dc->desc = "Fernvale UART controller";
    dc->reset = fernvale_uart_reset;
    dc->vmsd = &vmstate_fernvale_uart_regs;
    dc->props = fernvale_uart_properties;
}

static const TypeInfo fernvale_uart_info = {
    .name          = TYPE_FERNVALE_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FernvaleUARTState),
    .class_init    = fernvale_uart_class_init,
};


static QEMUMachine fernvale_machine = {
    .name = "fernvale",
    .desc = "Fernvale (ARM7EJ-S)",
    .init = fernvale_init,
};

static void fernvale_machine_init(void)
{
    qemu_register_machine(&fernvale_machine);
}

machine_init(fernvale_machine_init);

static void fernvale_register_types(void)
{
    type_register_static(&fernvale_uart_info);
#if 0
    type_register_static(&mv88w8618_pit_info);
    type_register_static(&mv88w8618_flashcfg_info);
    type_register_static(&mv88w8618_eth_info);
    type_register_static(&mv88w8618_wlan_info);
    type_register_static(&musicpal_lcd_info);
    type_register_static(&musicpal_gpio_info);
    type_register_static(&musicpal_key_info);
    type_register_static(&musicpal_misc_info);
#endif
}

type_init(fernvale_register_types)
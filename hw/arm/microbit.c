/*
 * micro:bit board and nrf51 soc emulation.
 *
 * Copyright (c) 2018 Zixuan Wang <zxwang42@gmail.com>
 *
 * This code is licensed under the GPL.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/arm.h"
#include "hw/arm/armv7m.h"
#include "hw/or-irq.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "hw/misc/unimp.h"
#include "hw/char/cmsdk-apb-uart.h"
#include "hw/timer/cmsdk-apb-timer.h"
#include "hw/devices.h"
#include "qemu/log.h"
#include "hw/loader.h"
#include "hw/block/flash.h"
#include "hw/ptimer.h"
#include "crypto/random.h"
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"

/**
 * MICROBIT LED MATRIX
 */

#define TYPE_MICROBIT_LED_MATRIX "microbit_led_matrix"
#define MICROBIT_LED_MATRIX(obj) \
    OBJECT_CHECK(MICROBITLedMatrixState, (obj), TYPE_MICROBIT_LED_MATRIX)

enum {
    MICROBIT_LED_MAP_MASK = 0x01FFFFFF,
    MICROBIT_LED_HSIZE = 10,
    MICROBIT_LED_VSIZE = 40,
    MICROBIT_LED_HSKIP = 40,
    MICROBIT_LED_VSKIP = 10,
    MICROBIT_LED_HBASE = 40,
    MICROBIT_LED_VBASE = 40,
    MICROBIT_LED_EVENT_NONE  = 0,
    MICROBIT_LED_EVENT_FRONT = 1,
    MICROBIT_LED_EVENT_BACK  = 2,
};

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    /* Only 25 bits are used */
    uint32_t led_state;
    uint8_t led_event;
    QemuConsole *con;

} MICROBITLedMatrixState;

static uint64_t microbit_led_matrix_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    MICROBITLedMatrixState *s = (MICROBITLedMatrixState *)opaque;
    s->led_event = MICROBIT_LED_EVENT_BACK | MICROBIT_LED_EVENT_FRONT;
    return s->led_state;
}

typedef struct {
    int x;
    int y;
} matrix_point_t;

static const matrix_point_t matrix_map[3 * 9] = {
    /* Row 2 Col 8 and 9 not used, set as (5,5) */
    {0,0},{4,2},{2,4},
    {2,0},{0,2},{4,4},
    {4,0},{2,2},{0,4},
    {4,3},{1,0},{0,1},
    {3,3},{3,0},{1,1},
    {2,3},{3,4},{2,1},
    {1,3},{1,4},{3,1},
    {0,3},{5,5},{4,1},
    {1,2},{5,5},{3,2},
};

static void microbit_led_matrix_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size)
{
    MICROBITLedMatrixState *s = (MICROBITLedMatrixState *)opaque;

    uint32_t clear_bits;
    uint32_t row_bits = (val >> 13) & 7;
    uint32_t col_bits = (~(val >> 4)) & 0x1FF;
    uint32_t led_bits = 0;
    int index;
    int row;

    switch (row_bits) {
        case 1:
            clear_bits = 0x000f8815;
            row = 0;
            break;
        case 2:
            clear_bits = 0x00a0540a;
            row = 1;
            break;
        case 4:
            clear_bits = 0x015023e0;
            row = 2;
            break;
        default:
            // printf("%s: abort due to wrong row bits %d\n", __func__, row_bits);
            return;
    };

    for (int col = 0; col < 9; col++) {
        if ((row == 1) && ((col == 8) || (col == 9)))
            continue;
        
        index = row + col * 3;
        if (col_bits & (1 << col))
            led_bits |= 1 << (matrix_map[index].x + matrix_map[index].y * 5);
    }

    s->led_state &= ~clear_bits;
    s->led_state |= led_bits;
    s->led_state &= MICROBIT_LED_MAP_MASK;
    // printf("%s: led_state 0x%08x\n", __func__, s->led_state);

    /* Redraw background and front */
    s->led_event = MICROBIT_LED_EVENT_BACK | MICROBIT_LED_EVENT_FRONT;
}

static const MemoryRegionOps microbit_led_matrix_mem_ops = {
    .read = microbit_led_matrix_read,
    .write = microbit_led_matrix_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void microbit_led_matrix_draw_block(DisplaySurface *ds,
                                           int ltx, int lty,
                                           int rbx, int rby,
                                           uint32_t color)
{
    int cx, cy, bpp;
    uint8_t *d;

    /**
     *                           x
     *    ----------------------->
     *   |   (ltx,lty)
     *   |       .----------.
     *   |       |          |
     *   |       |          |
     *   |       .----------.
     *   |              (rbx,rby)
     * y v
     * 
     */
    bpp = (surface_bits_per_pixel(ds) + 7) >> 3;
    for (cy = lty; cy <= rby; cy++) {
        d = surface_data(ds) + surface_stride(ds) * cy + bpp * ltx;
        switch (bpp) {
            case 1:
                for (cx = ltx; cx <= rbx; cx++) {
                    *((uint8_t *)d) = (uint8_t)color;
                    d++;
                }
                break;
            case 2:
                for (cx = ltx; cx <= rbx; cx++) {
                    *((uint16_t *)d) = (uint16_t)color;
                    d += 2;
                }
                break;
            case 4:
                for (cx = ltx; cx <= rbx; cx++) {
                    *((uint32_t *)d) = (uint32_t)color;
                    d += 4;
                }
                break;
            default:
                error_report("%s: cannot handle %d bits", __func__, bpp);
                exit(1);
                break;
        }
    }
}

static void microbit_led_matrix_update_display(void *opaque)
{
    MICROBITLedMatrixState *s = (MICROBITLedMatrixState *)opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    int bits_per_pixel = surface_bits_per_pixel(surf);
    uint32_t front_color;
    uint8_t *d1;
    int bpp;
    int y;
    int ltx, lty;
    int row, col;
    int i;

    switch (bits_per_pixel) {
        case 8:
            front_color = rgb_to_pixel8(0xFF, 0xFF, 0xFF);
            break;
        case 15:
            front_color = rgb_to_pixel15(0xFF, 0xFF, 0xFF);
            break;
        case 16:
            front_color = rgb_to_pixel16(0xFF, 0xFF, 0xFF);
            break;
        case 24:
            front_color = rgb_to_pixel24(0xFF, 0xFF, 0xFF);
            break;
        case 32:
            front_color = rgb_to_pixel32(0xFF, 0xFF, 0xFF);
            break;
        default:
            error_report("microbit internal error: " \
                         "[%s] can't handle %d bit color\n",
                         __func__, bits_per_pixel);
            exit(1);
    }

    /* Clear screen */
    if (s->led_event & MICROBIT_LED_EVENT_BACK) {
        bpp = (surface_bits_per_pixel(surf) + 7) >> 3;
        d1 = surface_data(surf);
        for (y = 0; y < surface_height(surf); y++) {
            memset(d1, 0x00, surface_width(surf) * bpp);
            d1 += surface_stride(surf);
        }
    }

    /* Render matrix */
    if (s->led_event & MICROBIT_LED_EVENT_FRONT) {
        for (i = 0; i < 25; i ++) {
            if (s->led_state & (1 << i)) {
                row = i / 5;
                col = i % 5;
                ltx = MICROBIT_LED_HBASE + 
                    col * (MICROBIT_LED_HSKIP + MICROBIT_LED_HSIZE);
                lty = MICROBIT_LED_VBASE +
                    row * (MICROBIT_LED_VSKIP + MICROBIT_LED_VSIZE);
                microbit_led_matrix_draw_block(surf,
                                               ltx, lty,
                                               ltx + MICROBIT_LED_HSIZE,
                                               lty + MICROBIT_LED_VSIZE,
                                               front_color);
            }
        }
    }

    s->led_event = MICROBIT_LED_EVENT_NONE;
    dpy_gfx_update(s->con, 0, 0,
                   surface_width(surf), surface_height(surf));
}

static void microbit_led_matrix_invalidate_display(void *opaque)
{
    MICROBITLedMatrixState *s = (MICROBITLedMatrixState *)opaque;
    s->led_event = MICROBIT_LED_EVENT_BACK | MICROBIT_LED_EVENT_FRONT;
}

static void microbit_led_matrix_text_update(void *opaque,
                                            console_ch_t *chardata)
{
    MICROBITLedMatrixState *s = (MICROBITLedMatrixState *)opaque;
    char buf[5];

    dpy_text_cursor(s->con, -1, -1);
    qemu_console_resize(s->con, 4, 1);

    snprintf(buf, 5, "%04hhx", s->led_state);
    for (int i = 0; i < 4; i++) {
        console_write_ch(chardata++, ATTR2CHTYPE(buf[i], QEMU_COLOR_BLUE,
                                                QEMU_COLOR_BLACK, 1));
    }
    dpy_text_update(s->con, 0, 0, 4, 1);
}

static int microbit_led_matrix_post_load(void *opaque, int version_id)
{
    microbit_led_matrix_invalidate_display(opaque);
    return 0;
}

static const VMStateDescription vmstate_microbit_led_matrix = {
    .name = TYPE_MICROBIT_LED_MATRIX,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = microbit_led_matrix_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(led_state, MICROBITLedMatrixState),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps microbit_led_matrix_graph_ops = {
    .invalidate  = microbit_led_matrix_invalidate_display,
    .gfx_update  = microbit_led_matrix_update_display,
    .text_update = microbit_led_matrix_text_update,
};

static void microbit_led_matrix_init(Object *obj)
{
    MICROBITLedMatrixState *s = MICROBIT_LED_MATRIX(obj);
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &microbit_led_matrix_mem_ops,
                          s, TYPE_MICROBIT_LED_MATRIX, 1);
    sysbus_init_mmio(dev, &s->iomem);
}

static void microbit_led_matrix_realize(DeviceState *dev, Error **errep)
{
    MICROBITLedMatrixState *s = MICROBIT_LED_MATRIX(dev);
    s->con = graphic_console_init(dev, 0, &microbit_led_matrix_graph_ops, s);
}

static void microbit_led_matrix_reset(DeviceState *d)
{
    MICROBITLedMatrixState *s = MICROBIT_LED_MATRIX(d);
    s->led_state = 0;
    s->led_event = MICROBIT_LED_EVENT_BACK | MICROBIT_LED_EVENT_FRONT;
    qemu_console_resize(s->con, 400, 400);
}

static void microbit_led_matrix_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = TYPE_MICROBIT_LED_MATRIX;
    dc->vmsd = &vmstate_microbit_led_matrix;
    dc->reset = microbit_led_matrix_reset;
    dc->realize = microbit_led_matrix_realize;
}

static const TypeInfo microbit_led_matrix_info = {
    .name = TYPE_MICROBIT_LED_MATRIX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MICROBITLedMatrixState),
    .instance_init = microbit_led_matrix_init,
    .class_init = microbit_led_matrix_class_init,
};

/**
 * NRF51 GPIO
 */

#define TYPE_NRF51_GPIO "nrf51_gpio"
#define NRF51_GPIO(obj) \
    OBJECT_CHECK(NRF51GPIOState, (obj), TYPE_NRF51_GPIO)

enum {
    NRF51_GPIO_OUT       = 0x504,
    NRF51_GPIO_OUTSET    = 0x508,
    NRF51_GPIO_OUTCLR    = 0x50C,
    NRF51_GPIO_IN        = 0x510,
    NRF51_GPIO_DIR       = 0x514,
    NRF51_GPIO_DIRSET    = 0x518,
    NRF51_GPIO_DIRCLR    = 0x51C,
    NRF51_GPIO_PIN_CNF0  = 0x700,
    NRF51_GPIO_PIN_CNF1  = 0x704,
    NRF51_GPIO_PIN_CNF2  = 0x708,
    NRF51_GPIO_PIN_CNF3  = 0x70C,
    NRF51_GPIO_PIN_CNF4  = 0x710,
    NRF51_GPIO_PIN_CNF5  = 0x714,
    NRF51_GPIO_PIN_CNF6  = 0x718,
    NRF51_GPIO_PIN_CNF7  = 0x71C,
    NRF51_GPIO_PIN_CNF8  = 0x720,
    NRF51_GPIO_PIN_CNF9  = 0x724,
    NRF51_GPIO_PIN_CNF10 = 0x728,
    NRF51_GPIO_PIN_CNF11 = 0x72C,
    NRF51_GPIO_PIN_CNF12 = 0x730,
    NRF51_GPIO_PIN_CNF13 = 0x734,
    NRF51_GPIO_PIN_CNF14 = 0x738,
    NRF51_GPIO_PIN_CNF15 = 0x73C,
    NRF51_GPIO_PIN_CNF16 = 0x740,
    NRF51_GPIO_PIN_CNF17 = 0x744,
    NRF51_GPIO_PIN_CNF18 = 0x748,
    NRF51_GPIO_PIN_CNF19 = 0x74C,
    NRF51_GPIO_PIN_CNF20 = 0x750,
    NRF51_GPIO_PIN_CNF21 = 0x754,
    NRF51_GPIO_PIN_CNF22 = 0x758,
    NRF51_GPIO_PIN_CNF23 = 0x75C,
    NRF51_GPIO_PIN_CNF24 = 0x760,
    NRF51_GPIO_PIN_CNF25 = 0x764,
    NRF51_GPIO_PIN_CNF26 = 0x768,
    NRF51_GPIO_PIN_CNF27 = 0x76C,
    NRF51_GPIO_PIN_CNF28 = 0x770,
    NRF51_GPIO_PIN_CNF29 = 0x774,
    NRF51_GPIO_PIN_CNF30 = 0x778,
    NRF51_GPIO_PIN_CNF31 = 0x77C,
};

enum {
    PIN_CNF_DIR_IN  = 0,
    PIN_CNF_DIR_OUT = 1,

    PIN_CNF_INPUT_CONNECT    = 0,
    PIN_CNF_INPUT_DISCONNECT = 1,

    PIN_CNF_PULL_DISABLED = 0,
    PIN_CNF_PULL_PULLDOWN = 1,
    PIN_CNF_PULL_PULLUP   = 3,

    PIN_CNF_DRIVE_S0S1 = 0,
    PIN_CNF_DRIVE_H0S1 = 1,
    PIN_CNF_DRIVE_S0H1 = 2,
    PIN_CNF_DRIVE_H0H1 = 3,
    PIN_CNF_DRIVE_D0S1 = 4,
    PIN_CNF_DRIVE_D0H1 = 5,
    PIN_CNF_DRIVE_S0D1 = 6,
    PIN_CNF_DRIVE_H0D1 = 7,

    PIN_CNF_SENSE_DISABLED = 0,
    PIN_CNF_SENSE_HIGH     = 2,
    PIN_CNF_SENSE_LOW      = 3,
};

typedef struct {
    uint32_t dir;
    uint32_t input;
    uint32_t pull;
    uint32_t drive;
    uint32_t sense;
} NRF51GPIOPin;

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    NRF51GPIOPin pin[32];
    uint32_t out;
    uint32_t in;
    uint32_t dir;
} NRF51GPIOState;

static const VMStateDescription vmstate_nrf51_gpio = {
    .name = TYPE_NRF51_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(out, NRF51GPIOState),
        VMSTATE_UINT32(in, NRF51GPIOState),
        VMSTATE_UINT32(dir, NRF51GPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static Property nrf51_gpio_properties[] = {
    DEFINE_PROP_UINT32("out", NRF51GPIOState, out, 0),
    DEFINE_PROP_UINT32("in", NRF51GPIOState, in, 0),
    DEFINE_PROP_UINT32("dir", NRF51GPIOState, dir, 0),
    DEFINE_PROP_END_OF_LIST()
};

static void nrf51_gpio_pin_cnf_write(NRF51GPIOPin *p, uint32_t cnf)
{
    p->dir   = (cnf >>  0) & 1;
    p->input = (cnf >>  1) & 1;
    p->pull  = (cnf >>  2) & 3;
    p->drive = (cnf >>  8) & 7;
    p->sense = (cnf >> 16) & 3;
}

static uint32_t nrf51_gpio_pin_cnf_read(NRF51GPIOPin *p)
{
    uint32_t cnf = 0;
    cnf |= p->dir   <<  0;
    cnf |= p->input <<  1;
    cnf |= p->pull  <<  2;
    cnf |= p->drive <<  8;
    cnf |= p->sense << 16;
    return cnf;
}

static void nrf51_gpio_pin_dir_update(NRF51GPIOState *s)
{
    for (int i = 0; i < 32; i++) {
        if (s->dir & (1 << i)) {
            if (s->pin[i].dir != PIN_CNF_DIR_IN)
                s->pin[i].dir = PIN_CNF_DIR_IN;
        } else {
            if (s->pin[i].dir != PIN_CNF_DIR_OUT)
                s->pin[i].dir = PIN_CNF_DIR_OUT;
        }
    }
}

static void nrf51_gpio_write_out(NRF51GPIOState *s)
{
    AddressSpace *as = cpu_get_address_space(CPU(first_cpu), 0);
    if (s->out & 0x0000FFF0) {
        stw_phys(as, 0x40020000, s->out & 0x0000FFF0);
    }
    s->out = 0;
}

static void nrf51_gpio_read_in(NRF51GPIOState *s)
{
    /* TODO: update */
}

static uint64_t nrf51_gpio_read(void *opaque, hwaddr offset,
                                unsigned size)
{
    NRF51GPIOState *s = (NRF51GPIOState *)opaque;

    switch (offset) {
        case NRF51_GPIO_OUT:
        case NRF51_GPIO_OUTSET:
        case NRF51_GPIO_OUTCLR:
            return s->out;
        case NRF51_GPIO_IN:
            nrf51_gpio_read_in(s);
            return s->in;
        case NRF51_GPIO_DIR:
        case NRF51_GPIO_DIRSET:
        case NRF51_GPIO_DIRCLR:
            return s->dir;
        case NRF51_GPIO_PIN_CNF0:
        case NRF51_GPIO_PIN_CNF1:
        case NRF51_GPIO_PIN_CNF2:
        case NRF51_GPIO_PIN_CNF3:
        case NRF51_GPIO_PIN_CNF4:
        case NRF51_GPIO_PIN_CNF5:
        case NRF51_GPIO_PIN_CNF6:
        case NRF51_GPIO_PIN_CNF7:
        case NRF51_GPIO_PIN_CNF8:
        case NRF51_GPIO_PIN_CNF9:
        case NRF51_GPIO_PIN_CNF10:
        case NRF51_GPIO_PIN_CNF11:
        case NRF51_GPIO_PIN_CNF12:
        case NRF51_GPIO_PIN_CNF13:
        case NRF51_GPIO_PIN_CNF14:
        case NRF51_GPIO_PIN_CNF15:
        case NRF51_GPIO_PIN_CNF16:
        case NRF51_GPIO_PIN_CNF17:
        case NRF51_GPIO_PIN_CNF18:
        case NRF51_GPIO_PIN_CNF19:
        case NRF51_GPIO_PIN_CNF20:
        case NRF51_GPIO_PIN_CNF21:
        case NRF51_GPIO_PIN_CNF22:
        case NRF51_GPIO_PIN_CNF23:
        case NRF51_GPIO_PIN_CNF24:
        case NRF51_GPIO_PIN_CNF25:
        case NRF51_GPIO_PIN_CNF26:
        case NRF51_GPIO_PIN_CNF27:
        case NRF51_GPIO_PIN_CNF28:
        case NRF51_GPIO_PIN_CNF29:
        case NRF51_GPIO_PIN_CNF30:
        case NRF51_GPIO_PIN_CNF31:
            return nrf51_gpio_pin_cnf_read(&s->pin[(offset >> 2) & 0x1f]);
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: reading a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            return 0;
    };
}

static void nrf51_gpio_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    NRF51GPIOState *s = (NRF51GPIOState *)opaque;
    int index;

    switch (offset) {
        case NRF51_GPIO_OUT:
            s->out = value & s->dir;
            nrf51_gpio_write_out(s);
            break;
        case NRF51_GPIO_OUTSET:
            s->out |= value & s->dir;
            nrf51_gpio_write_out(s);
            break;
        case NRF51_GPIO_OUTCLR:
            s->out &= ~((uint32_t)value) & s->dir;
            nrf51_gpio_write_out(s);
            break;
        case NRF51_GPIO_DIR:
            s->dir = value;
            nrf51_gpio_pin_dir_update(s);
            break;
        case NRF51_GPIO_DIRSET:
            s->dir |= value;
            nrf51_gpio_pin_dir_update(s);
            break;
        case NRF51_GPIO_DIRCLR:
            s->dir &= ~((uint32_t)value);
            nrf51_gpio_pin_dir_update(s);
            break;
        case NRF51_GPIO_PIN_CNF0:
        case NRF51_GPIO_PIN_CNF1:
        case NRF51_GPIO_PIN_CNF2:
        case NRF51_GPIO_PIN_CNF3:
        case NRF51_GPIO_PIN_CNF4:
        case NRF51_GPIO_PIN_CNF5:
        case NRF51_GPIO_PIN_CNF6:
        case NRF51_GPIO_PIN_CNF7:
        case NRF51_GPIO_PIN_CNF8:
        case NRF51_GPIO_PIN_CNF9:
        case NRF51_GPIO_PIN_CNF10:
        case NRF51_GPIO_PIN_CNF11:
        case NRF51_GPIO_PIN_CNF12:
        case NRF51_GPIO_PIN_CNF13:
        case NRF51_GPIO_PIN_CNF14:
        case NRF51_GPIO_PIN_CNF15:
        case NRF51_GPIO_PIN_CNF16:
        case NRF51_GPIO_PIN_CNF17:
        case NRF51_GPIO_PIN_CNF18:
        case NRF51_GPIO_PIN_CNF19:
        case NRF51_GPIO_PIN_CNF20:
        case NRF51_GPIO_PIN_CNF21:
        case NRF51_GPIO_PIN_CNF22:
        case NRF51_GPIO_PIN_CNF23:
        case NRF51_GPIO_PIN_CNF24:
        case NRF51_GPIO_PIN_CNF25:
        case NRF51_GPIO_PIN_CNF26:
        case NRF51_GPIO_PIN_CNF27:
        case NRF51_GPIO_PIN_CNF28:
        case NRF51_GPIO_PIN_CNF29:
        case NRF51_GPIO_PIN_CNF30:
        case NRF51_GPIO_PIN_CNF31:
            index = (offset >> 2) & 0x1f;
            s->dir |= (value & 1) << index;
            nrf51_gpio_pin_cnf_write(&s->pin[index], value);
            break;
        case NRF51_GPIO_IN:
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: writing a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            break;
    };
}

static const MemoryRegionOps nrf51_gpio_ops = {
    .read = nrf51_gpio_read,
    .write = nrf51_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void nrf51_gpio_init(Object *obj)
{
    NRF51GPIOState *s = NRF51_GPIO(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &nrf51_gpio_ops, s,
                          TYPE_NRF51_GPIO, 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = nrf51_gpio_properties;
    dc->vmsd = &vmstate_nrf51_gpio;
}

static const TypeInfo nrf51_gpio_info = {
    .name          = TYPE_NRF51_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NRF51GPIOState),
    .instance_init = nrf51_gpio_init,
    .class_init    = nrf51_gpio_class_init,
};

/**
 * NRF51 RNG
 *   Random Number Generator
 *   NOTE: ought to raise irq, but microbit does not handle it
 *         so ommit the irq implementation
 */

#define TYPE_NRF51_RNG "nrf51_rng"
#define NRF51_RNG(obj) \
    OBJECT_CHECK(NRF51RNGState, (obj), TYPE_NRF51_RNG)

enum {
    NRF51_RNG_START    = 0x000,
    NRF51_RNG_STOP     = 0x004,
    NRF51_RNG_VALRDY   = 0x100,
    NRF51_RNG_SHORTS   = 0x200,
    NRF51_RNG_INTEN    = 0x300,
    NRF51_RNG_INTENSET = 0x304,
    NRF51_RNG_INTENCLR = 0x308,
    NRF51_RNG_CONFIG   = 0x504,
    NRF51_RNG_VALUE    = 0x508,
};

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    uint8_t value;
    uint32_t config;
    bool ready;
    bool started;
} NRF51RNGState;

static const VMStateDescription vmstate_nrf51_rng = {
    .name = TYPE_NRF51_RNG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(value, NRF51RNGState),
        VMSTATE_UINT32(config, NRF51RNGState),
        VMSTATE_BOOL(ready, NRF51RNGState),
        VMSTATE_BOOL(started, NRF51RNGState),
        VMSTATE_END_OF_LIST()
    }
};

static Property nrf51_rng_properties[] = {
    DEFINE_PROP_UINT8("value", NRF51RNGState, value, 0),
    DEFINE_PROP_UINT32("config", NRF51RNGState, config, 0),
    DEFINE_PROP_BOOL("ready", NRF51RNGState, ready, false),
    DEFINE_PROP_BOOL("started", NRF51RNGState, started, false),
    DEFINE_PROP_END_OF_LIST()
};

static uint64_t nrf51_rng_read(void *opaque, hwaddr offset,
                               unsigned size)
{
    NRF51RNGState *s = (NRF51RNGState *)opaque;

    switch (offset) {
        case NRF51_RNG_START:
            return s->started;
        case NRF51_RNG_STOP:
            return s->started;
        case NRF51_RNG_VALRDY:
            /* Always ready, actually generated when reading VALUE*/
            return (s->started) && 1; 
        case NRF51_RNG_SHORTS:
        case NRF51_RNG_INTEN:
        case NRF51_RNG_INTENSET:
        case NRF51_RNG_INTENCLR:
            qemu_log_mask(LOG_UNIMP,
                          "%s: writing unimp offset 0x%x\n",
                          __func__,
                          (int)offset);
            return 0;
        case NRF51_RNG_VALUE:
            qcrypto_random_bytes(&s->value, 1, &error_fatal);
            return s->value;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: reading a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            return 0;
    }
}

static void nrf51_rng_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    NRF51RNGState *s = (NRF51RNGState *)opaque;

    switch (offset) {
        case NRF51_RNG_START:
            s->started = true;
            break;
        case NRF51_RNG_STOP:
            s->started = false;
            break;
        case NRF51_RNG_CONFIG:
            s->config = value & 1;
            break;
        case NRF51_RNG_SHORTS:
        case NRF51_RNG_INTEN:
        case NRF51_RNG_INTENSET:
        case NRF51_RNG_INTENCLR:
        case NRF51_RNG_VALRDY:
            qemu_log_mask(LOG_UNIMP,
                          "%s: writing unimp offset 0x%x\n",
                          __func__,
                          (int)offset);
            break;
        case NRF51_RNG_VALUE:
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: writing a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            break;
    }
}

static const MemoryRegionOps nrf51_rng_ops = {
    .read = nrf51_rng_read,
    .write = nrf51_rng_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void nrf51_rng_init(Object *obj)
{
    NRF51RNGState *s = NRF51_RNG(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &nrf51_rng_ops, s,
                          TYPE_NRF51_RNG, 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_rng_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = nrf51_rng_properties;
    dc->vmsd = &vmstate_nrf51_rng;
}

static const TypeInfo nrf51_rng_info = {
    .name          = TYPE_NRF51_RNG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NRF51RNGState),
    .instance_init = nrf51_rng_init,
    .class_init    = nrf51_rng_class_init,
};

/**
 * NRF51 NVMC
 *   Non-Volatile Memory Controller
 */

#define TYPE_NRF51_NVMC "nrf51_nvmc"
#define NRF51_NVMC(obj) \
    OBJECT_CHECK(NRF51NVMCState, (obj), TYPE_NRF51_NVMC)

enum{
    NRF51_NVMC_READY     = 0x400,
    NRF51_NVMC_CONFIG    = 0x504,
    NRF51_NVMC_ERASEPAGE = 0x508,
    NRF51_NVMC_ERASEPCR1 = 0x508,
    NRF51_NVMC_ERASEALL  = 0x50C,
    NRF51_NVMC_ERASEPCR0 = 0x510,
    NRF51_NVMC_ERASEUICR = 0x514,
};

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    uint32_t ready;
    uint32_t config;
} NRF51NVMCState;

static uint64_t nrf51_nvmc_read(void *opaque, hwaddr offset,
                                unsigned size)
{
    NRF51NVMCState *s = (NRF51NVMCState *)opaque;
    
    switch (offset) {
        case NRF51_NVMC_READY:
            return s->ready;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: reading a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            return 0;
    }
}

static void nrf51_nvmc_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    NRF51NVMCState *s = (NRF51NVMCState *)opaque;
    
    switch (offset) {
        case NRF51_NVMC_CONFIG:
            s->config = value;
            break;
        case NRF51_NVMC_READY:
        case NRF51_NVMC_ERASEPAGE:
        /* case NRF51_NVMC_ERASEPCR1: OVERLAPPED */
        case NRF51_NVMC_ERASEALL:
        case NRF51_NVMC_ERASEPCR0:
        case NRF51_NVMC_ERASEUICR:
            qemu_log_mask(LOG_UNIMP,
                          "%s: writing unimp offset 0x%x\n",
                          __func__,
                          (int)offset);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: writing a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            break;
    }
}

static const MemoryRegionOps nrf51_nvmc_ops = {
    .read = nrf51_nvmc_read,
    .write = nrf51_nvmc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_nrf51_nvmc = {
    .name = TYPE_NRF51_NVMC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ready, NRF51NVMCState),
        VMSTATE_UINT32(config, NRF51NVMCState),
        VMSTATE_END_OF_LIST()
    }
};

static Property nrf51_nvmc_properties[] = {
    DEFINE_PROP_UINT32("ready", NRF51NVMCState, ready, 1),
    DEFINE_PROP_UINT32("config", NRF51NVMCState, config, 0),
    DEFINE_PROP_END_OF_LIST()
};

static void nrf51_nvmc_init(Object *obj)
{
    NRF51NVMCState *s = NRF51_NVMC(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &nrf51_nvmc_ops, s,
                          TYPE_NRF51_NVMC, 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_nvmc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = nrf51_nvmc_properties;
    dc->vmsd = &vmstate_nrf51_nvmc;
}

static const TypeInfo nrf51_nvmc_info = {
    .name          = TYPE_NRF51_NVMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NRF51NVMCState),
    .instance_init = nrf51_nvmc_init,
    .class_init    = nrf51_nvmc_class_init,
};

/**
 * NRF51 FICR
 */

#define TYPE_NRF51_FICR "nrf51_ficr"
#define NRF51_FICR(obj) \
    OBJECT_CHECK(NRF51FICRState, (obj), TYPE_NRF51_FICR)

enum{
    NRF51_FICR_CODEPAGESIZE = 0x010,
    NRF51_FICR_CODESIZE = 0x014,
    NRF51_FICR_CLENR0 = 0x028,
    NRF51_FICR_PPFC = 0x02C,
    NRF51_FICR_NUMRAMBLOCK = 0x034,
    NRF51_FICR_SIZERAMBLOCKS = 0x038,
    NRF51_FICR_SIZERAMBLOCK0 = 0x038,
    NRF51_FICR_SIZERAMBLOCK1 = 0x03C,
    NRF51_FICR_SIZERAMBLOCK2 = 0x040,
    NRF51_FICR_SIZERAMBLOCK3 = 0x044,
    NRF51_FICR_CONFIGID = 0x05C,
    NRF51_FICR_DEVICEID0 = 0x060,
    NRF51_FICR_DEVICEID1 = 0x064,
    NRF51_FICR_ER0 = 0x080,
    NRF51_FICR_ER1 = 0x084,
    NRF51_FICR_ER2 = 0x088,
    NRF51_FICR_ER3 = 0x08C,
    NRF51_FICR_IR0 = 0x090,
    NRF51_FICR_IR1 = 0x094,
    NRF51_FICR_IR2 = 0x098,
    NRF51_FICR_IR3 = 0x09C,
    NRF51_FICR_DEVICEADDRTYPE = 0x0A0,
    NRF51_FICR_DEVICEADDR0 = 0x0A4,
    NRF51_FICR_DEVICEADDR1 = 0x0A8,
    NRF51_FICR_OVERRIDEEN = 0x0AC,
    NRF51_FICR_NRF_1MBIT0 = 0x0B0,
    NRF51_FICR_NRF_1MBIT1 = 0x0B4,
    NRF51_FICR_NRF_1MBIT2 = 0x0B8,
    NRF51_FICR_NRF_1MBIT3 = 0x0BC,
    NRF51_FICR_NRF_1MBIT4 = 0x0C0,
    NRF51_FICR_BLE_1MBIT0 = 0x0EC,
    NRF51_FICR_BLE_1MBIT1 = 0x0F0,
    NRF51_FICR_BLE_1MBIT2 = 0x0F4,
    NRF51_FICR_BLE_1MBIT3 = 0x0F8,
    NRF51_FICR_BLE_1MBIT4 = 0x0FC,
};

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    uint32_t codepagesize;
    uint32_t codesize;

} NRF51FICRState;

static uint64_t nrf51_ficr_read(void *opaque, hwaddr offset,
                                unsigned size)
{
    NRF51FICRState *s = (NRF51FICRState *)opaque;

    switch (offset) {
        case NRF51_FICR_CODEPAGESIZE:
            return s->codepagesize;
        case NRF51_FICR_CODESIZE:
            return s->codesize;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: reading a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            return 0;
    };
}

static void nrf51_ficr_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    NRF51FICRState *s = (NRF51FICRState *)opaque;

    switch (offset) {
        case NRF51_FICR_CODEPAGESIZE:
            s->codepagesize = value;
            break;
        case NRF51_FICR_CODESIZE:
            s->codesize = value;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: writing a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            break;
    };
}

static const MemoryRegionOps nrf51_ficr_ops = {
    .read = nrf51_ficr_read,
    .write = nrf51_ficr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_nrf51_ficr = {
    .name = TYPE_NRF51_FICR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(codepagesize, NRF51FICRState),
        VMSTATE_UINT32(codesize, NRF51FICRState),
        VMSTATE_END_OF_LIST()
    }
};

static Property nrf51_ficr_properties[] = {
    DEFINE_PROP_UINT32("codepagesize", NRF51FICRState, codepagesize, 4096),
    DEFINE_PROP_UINT32("codesize", NRF51FICRState, codesize, 64),
    DEFINE_PROP_END_OF_LIST()
};

static void nrf51_ficr_init(Object *obj)
{
    NRF51FICRState *s = NRF51_FICR(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &nrf51_ficr_ops, s,
                          TYPE_NRF51_FICR, 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_ficr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = nrf51_ficr_properties;
    dc->vmsd = &vmstate_nrf51_ficr;
}

static const TypeInfo nrf51_ficr_info = {
    .name          = TYPE_NRF51_FICR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NRF51FICRState),
    .instance_init = nrf51_ficr_init,
    .class_init    = nrf51_ficr_class_init,
};

/**
 * NRF51 CLOCK & POWER & MPU
 *   With respect to nRF51822 Reference Manual
 *   NOTE: incomplete implementation
 *         timer does not need clock input,
 *         so the clock is a fake one
 */

#define TYPE_NRF51_CPM "nrf51_clock_power_mpu"
#define NRF51_CPM(obj) \
    OBJECT_CHECK(NRF51CPMState, (obj), TYPE_NRF51_CPM)

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;

    /* Clock */
    bool hfclk_enabled;
    bool lfclk_enabled;

    /* Power */
    bool ramon;
} NRF51CPMState;

enum {
    NRF51_CLK_HFCLKSTART   = 0x000,
    NRF51_CLK_HFCLKSTOP    = 0x004,
    NRF51_CLK_LFCLKSTART   = 0x008,
    NRF51_CLK_LFCLKSTOP    = 0x00C,
    NRF51_CLK_HFCLKSTARTED = 0x100,
    NRF51_CLK_LFCLKSTARTED = 0x104,
    NRF51_CLK_CAL          = 0x010,
    NRF51_CLK_CTSTART      = 0x014,
    NRF51_CLK_CTSTOP       = 0x018,
    NRF51_CLK_INTENSET     = 0x304,
    NRF51_CLK_INTENCLR     = 0x308,
    NRF51_CLK_HFCLKRUN     = 0x408,
    NRF51_CLK_HFCLKSTAT    = 0x40c,
    NRF51_CLK_LFCLKRUN     = 0x414,
    NRF51_CLK_LFCLKSTAT    = 0x418,
    NRF51_CLK_LFCLKSRCCOPY = 0x41c,
    NRF51_CLK_LFCLKSRC     = 0x518,
    NRF51_CLK_CTIV         = 0x538,
    NRF51_CLK_XTALFREQ     = 0x550,
    NRF51_PWR_RAMON        = 0x524,
    NRF51_UNKNOWN_VAL      = 0,
};

static uint64_t nrf51_cpm_read(void *opaque, hwaddr offset,
                               unsigned size)
{
    NRF51CPMState *s = (NRF51CPMState *)opaque;

    switch (offset) {
        case NRF51_CLK_HFCLKSTART:
        case NRF51_CLK_LFCLKSTART:
        case NRF51_CLK_HFCLKSTOP:
        case NRF51_CLK_LFCLKSTOP:
            return 0;
        case NRF51_CLK_HFCLKSTARTED:
            return s->hfclk_enabled;
        case NRF51_CLK_LFCLKSTARTED:
            return s->lfclk_enabled;
        case NRF51_PWR_RAMON:
            return s->ramon;
        case NRF51_CLK_LFCLKSRC:
            return NRF51_UNKNOWN_VAL;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: reading a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            return 0;
    };
}

static void nrf51_cpm_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    NRF51CPMState *s = (NRF51CPMState *)opaque;

    switch (offset) {
        case NRF51_CLK_HFCLKSTART:
            s->hfclk_enabled = (value & 1);
            break;
        case NRF51_CLK_LFCLKSTART:
            s->lfclk_enabled = (value & 1);
            break;
        case NRF51_CLK_HFCLKSTOP:
            s->hfclk_enabled = (value & 1) ? false : true;
            break;
        case NRF51_CLK_LFCLKSTOP:
            s->lfclk_enabled = (value & 1) ? false : true;
            break;
        case NRF51_CLK_HFCLKSTARTED:
        case NRF51_CLK_LFCLKSTARTED:
            break;
        case NRF51_PWR_RAMON:
            s->ramon = value & 0x00030003;
            break;
        case NRF51_CLK_LFCLKSRC:
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                            "%s: writing a bad offset 0x%x\n",
                            __func__,
                            (int)offset);
            break;
    };
}

static const MemoryRegionOps nrf51_cpm_ops = {
    .read = nrf51_cpm_read,
    .write = nrf51_cpm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_nrf51_cpm = {
    .name = TYPE_NRF51_CPM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(hfclk_enabled, NRF51CPMState),
        VMSTATE_BOOL(lfclk_enabled, NRF51CPMState),
        VMSTATE_END_OF_LIST()
    }
};

static Property nrf51_cpm_properties[] = {
    DEFINE_PROP_BOOL("hfclk_enabled", NRF51CPMState, hfclk_enabled, false),
    DEFINE_PROP_BOOL("lfclk_enabled", NRF51CPMState, lfclk_enabled, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void nrf51_cpm_init(Object *obj)
{
    NRF51CPMState *s = NRF51_CPM(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &nrf51_cpm_ops, s,
                          TYPE_NRF51_CPM, 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_cpm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = nrf51_cpm_properties;
    dc->vmsd = &vmstate_nrf51_cpm;
}

static const TypeInfo nrf51_cpm_info = {
    .name          = TYPE_NRF51_CPM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NRF51CPMState),
    .instance_init = nrf51_cpm_init,
    .class_init    = nrf51_cpm_class_init,
};

/**
 * NRF51 Timer
 *   With respect to nRF51822 Reference Manual
 */

#define TYPE_NRF51_TIMER "nrf51_timer"
#define NRF51_TIMER(obj) \
    OBJECT_CHECK(NRF51TimerState, (obj), TYPE_NRF51_TIMER)

#define NRF51_TIMER_BASE_FREQ 0x01000000

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    bool pulsed;
    uint32_t inten;
    uint32_t limit_mask;
    
    /**
     * freq = 16MHz / (2 ^ prescaler)
     * 0 <= prescaler <= 9
     * i.e:
     *   16MHz  <= freq <= 32KHz
     *   62.5ns <= tick <= 31.25us
     */
    uint32_t freq;

    /* Public Regs */
    uint32_t start;
    uint32_t stop;
    uint32_t count;
    uint32_t clear;
    uint32_t shutdown;
    uint32_t capture[4];
    uint32_t compare[4];
    uint32_t shorts;
    uint32_t intenset;
    uint32_t intenclr;
    uint32_t mode;
    uint32_t bitmode;
    uint32_t prescaler;
    uint32_t cc[4];

    /* Internal Regs */
    uint32_t internal_counter;
} NRF51TimerState;

enum {
    NRF51_TIMER_START     = 0x000,
    NRF51_TIMER_STOP      = 0x004,
    NRF51_TIMER_COUNT     = 0x008,
    NRF51_TIMER_CLEAR     = 0x00C,
    NRF51_TIMER_SHUTDOWN  = 0x010,
    NRF51_TIMER_CAPTURE0  = 0x040,
    NRF51_TIMER_CAPTURE1  = 0x044,
    NRF51_TIMER_CAPTURE2  = 0x048,
    NRF51_TIMER_CAPTURE3  = 0x04C,
    NRF51_TIMER_COMPARE0  = 0x140,
    NRF51_TIMER_COMPARE1  = 0x144,
    NRF51_TIMER_COMPARE2  = 0x148,
    NRF51_TIMER_COMPARE3  = 0x14C,
    NRF51_TIMER_SHORTS    = 0x200,
    NRF51_TIMER_INTENSET  = 0x304,
    NRF51_TIMER_INTENCLR  = 0x308,
    NRF51_TIMER_MODE      = 0x504,
    NRF51_TIMER_BITMODE   = 0x508,
    NRF51_TIMER_PRESCALER = 0x510,
    NRF51_TIMER_CC0       = 0x540,
    NRF51_TIMER_CC1       = 0x544,
    NRF51_TIMER_CC2       = 0x548,
    NRF51_TIMER_CC3       = 0x54C,
};

static uint64_t nrf51_timer_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    NRF51TimerState *s = (NRF51TimerState *)opaque;

    switch (offset) {
        case NRF51_TIMER_START:
            return s->start;
        case NRF51_TIMER_STOP:
            return s->stop;
        case NRF51_TIMER_COUNT:
            return s->count;
        case NRF51_TIMER_CLEAR:
            return s->clear;
        case NRF51_TIMER_SHUTDOWN:
            return s->shutdown;
        case NRF51_TIMER_CAPTURE0:
        case NRF51_TIMER_CAPTURE1:
        case NRF51_TIMER_CAPTURE2:
        case NRF51_TIMER_CAPTURE3:
            return s->capture[(offset >> 2) & 3];
        case NRF51_TIMER_COMPARE0:
        case NRF51_TIMER_COMPARE1:
        case NRF51_TIMER_COMPARE2:
        case NRF51_TIMER_COMPARE3:
            return s->compare[(offset >> 2) & 3];
        case NRF51_TIMER_SHORTS:
            return s->shorts;
        case NRF51_TIMER_INTENSET:
        case NRF51_TIMER_INTENCLR:
            qemu_log_mask(LOG_UNIMP,
                          "%s: `INTEN` not implemented when reading 0x%x\n",
                          __func__,
                          (int)offset);
            return 0;
        case NRF51_TIMER_MODE:
            return s->mode;
        case NRF51_TIMER_BITMODE:
            return s->bitmode;
        case NRF51_TIMER_PRESCALER:
            return s->prescaler;
        case NRF51_TIMER_CC0:
        case NRF51_TIMER_CC1:
        case NRF51_TIMER_CC2:
        case NRF51_TIMER_CC3:
            return s->cc[(offset >> 2) & 3];
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: reading a bad offset 0x%x\n",
                          __func__,
                          (int)offset);
            return 0;
    };
}

static void nrf51_timer_recalibrate(NRF51TimerState *s, int reload)
{
    uint32_t limit = 0xffff;
    if (s->mode & 1) {
        /* Counter mode */
        limit = s->count;
    } else {
        /* Timer mode */
        limit = 0;
    }
    ptimer_set_limit(s->timer, (uint64_t)limit, reload);
}

static void nrf51_timer_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    NRF51TimerState *s = (NRF51TimerState *)opaque;

    switch (offset) {
        case NRF51_TIMER_START: /* Start */
            if (value & 1) {
                ptimer_set_freq(s->timer, s->freq);
                switch (s->bitmode) {
                    case 0:
                        s->limit_mask = 0xffff;
                        break;
                    case 1:
                        s->limit_mask = 0xff;
                        break;
                    case 2:
                        s->limit_mask = 0xffffff;
                        break;
                    case 3:
                        s->limit_mask = 0xffffffff;
                        break;
                    default:
                        g_assert_not_reached();
                        break;
                }
                if (s->pulsed)
                    s->pulsed = false;
                else
                    nrf51_timer_recalibrate(s, 1);
                ptimer_run(s->timer, 0);
            }
            break;
        case NRF51_TIMER_STOP: /* Stop */
            if (value & 1) {
                ptimer_stop(s->timer);
                s->pulsed = true;
            }
            break;
        case NRF51_TIMER_COUNT: /* Count */
            printf("%s: set count to %d\n", __func__, s->count);
            if (s->mode & 1) {
                s->count = value;
                nrf51_timer_recalibrate(s, 1);
            }
            break;
        case NRF51_TIMER_CLEAR: /* Clear */
            if (value & 1) {
                s->internal_counter = 0;
                nrf51_timer_recalibrate(s, 1);
            }
            break;
        case NRF51_TIMER_SHUTDOWN: /* Shutdown */
            if (value & 1) {
                ptimer_stop(s->timer);
                s->internal_counter = 0;
                nrf51_timer_recalibrate(s, 1);
                s->pulsed = false;
            }
            break;
        case NRF51_TIMER_CAPTURE0:
        case NRF51_TIMER_CAPTURE1:
        case NRF51_TIMER_CAPTURE2:
        case NRF51_TIMER_CAPTURE3:
            s->cc[(offset >> 2) & 0x3] = s->internal_counter;
            break;
        case NRF51_TIMER_COMPARE0:
        case NRF51_TIMER_COMPARE1:
        case NRF51_TIMER_COMPARE2:
        case NRF51_TIMER_COMPARE3:
            s->compare[(offset >> 2) & 0x3] = value;
            break;
        case NRF51_TIMER_SHORTS: /* Shorts */
            qemu_log_mask(LOG_UNIMP,
                          "%s: `SHORTS` not implemented when writing 0x%x\n",
                          __func__,
                          (int)offset);
            break;
        case NRF51_TIMER_INTENSET: /* Intenset */
            s->inten |= (value >> 16) & 0xf;
            break;
        case NRF51_TIMER_INTENCLR:
            /* TODO: verify this formula */
            s->inten &= (~(value >> 16)) & 0xf;
            break;
        case NRF51_TIMER_MODE:
            s->mode = value & 1;
            nrf51_timer_recalibrate(s, 1);
            break;
        case NRF51_TIMER_BITMODE:
            s->bitmode = value & 0x3;
            break;
        case NRF51_TIMER_PRESCALER:
            s->prescaler = value & 0xf;
            nrf51_timer_recalibrate(s, 1);
            break;
        case NRF51_TIMER_CC0:
        case NRF51_TIMER_CC1:
        case NRF51_TIMER_CC2:
        case NRF51_TIMER_CC3:
            s->cc[(offset >> 2) & 0x3] = value;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: writing a bad offset 0x%x\n",
                          __func__,
                          (int)offset);
            break;
    };
}

static void nrf51_timer_tick(void *opaque)
{
    NRF51TimerState *s = (NRF51TimerState *)opaque;
    int i;

    s->internal_counter = (s->internal_counter + 1) & s->limit_mask;

    if (s->mode == 1) {
        /* Counter mode */
        if (s->internal_counter == s->count){
            s->internal_counter = 0;
            qemu_irq_pulse(s->irq);
        } else {
            qemu_irq_lower(s->irq);
        }
    } else {
        /* Timer mode */
        for (i = 0; i < 4; i++) {
            if (s->inten & (1 << i)) {
                if (s->cc[i] == s->internal_counter) {
                    s->compare[i] ++;
                    qemu_irq_pulse(s->irq);
                } else {
                    qemu_irq_lower(s->irq);
                }
            }
        }
    }
    
}

static const MemoryRegionOps nrf51_timer_ops = {
    .read = nrf51_timer_read,
    .write = nrf51_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_nrf51_timer = {
    .name = TYPE_NRF51_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PTIMER(timer, NRF51TimerState),
        VMSTATE_BOOL(pulsed, NRF51TimerState),
        VMSTATE_UINT32(inten, NRF51TimerState),
        VMSTATE_UINT32(limit_mask, NRF51TimerState),
        VMSTATE_UINT32(start, NRF51TimerState),
        VMSTATE_UINT32(stop, NRF51TimerState),
        VMSTATE_UINT32(count, NRF51TimerState),
        VMSTATE_UINT32(clear, NRF51TimerState),
        VMSTATE_UINT32(shutdown, NRF51TimerState),
        VMSTATE_UINT32_ARRAY(capture, NRF51TimerState, 4),
        VMSTATE_UINT32_ARRAY(compare, NRF51TimerState, 4),
        VMSTATE_UINT32(shorts, NRF51TimerState),
        VMSTATE_UINT32(intenset, NRF51TimerState),
        VMSTATE_UINT32(intenclr, NRF51TimerState),
        VMSTATE_UINT32(mode, NRF51TimerState),
        VMSTATE_UINT32(bitmode, NRF51TimerState),
        VMSTATE_UINT32(prescaler, NRF51TimerState),
        VMSTATE_UINT32_ARRAY(cc, NRF51TimerState, 4),
        VMSTATE_UINT32(internal_counter, NRF51TimerState),
        VMSTATE_END_OF_LIST()
    }
};

static Property nrf51_timer_properties[] = {
    DEFINE_PROP_UINT32("freq", NRF51TimerState, freq, NRF51_TIMER_BASE_FREQ),
    DEFINE_PROP_END_OF_LIST(),
};

static void nrf51_timer_realize(DeviceState *dev, Error **errp)
{
    NRF51TimerState *s = NRF51_TIMER(dev);
    QEMUBH *bh = qemu_bh_new(nrf51_timer_tick, s);
    s->timer = ptimer_init(bh, PTIMER_POLICY_DEFAULT | PTIMER_POLICY_CONTINUOUS_TRIGGER);
    s->freq = NRF51_TIMER_BASE_FREQ;
}

static void nrf51_timer_init(Object *obj)
{
    NRF51TimerState *s = NRF51_TIMER(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    sysbus_init_irq(sdb, &s->irq);
    memory_region_init_io(&s->iomem, obj, &nrf51_timer_ops, s,
                          TYPE_NRF51_TIMER, 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = nrf51_timer_realize;
    dc->props = nrf51_timer_properties;
    dc->vmsd = &vmstate_nrf51_timer;
}

static const TypeInfo nrf51_timer_info = {
    .name          = TYPE_NRF51_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NRF51TimerState),
    .instance_init = nrf51_timer_init,
    .class_init    = nrf51_timer_class_init,
};

static void nrf51_peri_init_types(void)
{
    type_register_static(&microbit_led_matrix_info);
    type_register_static(&nrf51_gpio_info);
    type_register_static(&nrf51_rng_info);
    type_register_static(&nrf51_nvmc_info);
    type_register_static(&nrf51_ficr_info);
    type_register_static(&nrf51_cpm_info);
    type_register_static(&nrf51_timer_info);    
}

type_init(nrf51_peri_init_types)


/**
 * micro:bit machine type
 */


typedef struct {
    /* Private */
    MachineState parent;

    /* Public */
    ARMv7MState armv7m;

} MICROBITMachineState;

typedef struct {
    /* Private */
    MachineClass parent;

    /* Public */
} MICROBITMachineClass;

#define TYPE_MICROBIT_MACHINE "micro:bit"
#define MICROBIT_MACHINE(obj) \
    OBJECT_CHECK(MICROBITMachineState, (obj), TYPE_MICROBIT_MACHINE)

#define MICROBIT_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(MICROBITMachineClass, (obj), TYPE_MICROBIT_MACHINE)

#define MICROBIT_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(MICROBITMachineClass, (klass), TYPE_MICROBIT_MACHINE)

enum {
    STARTUP_ADDR = 0x00018000,
    VECTOR_SIZE  = 0xC0,
    NUM_IRQ      = 64,

    /* Memory regions */
    CODE_LOADER_BASE = 0x00000000,
    CODE_LOADER_SIZE = 0x00018000,
    CODE_KERNEL_BASE = 0x00018000,
    CODE_KERNEL_SIZE = 0x00028000,
    FLASH_SEC_SIZE   = 0x00008000,
    RAM_BASE         = 0x20000000,

    /* ABP Peripherals */
    ABP_PERI_BASE = 0x40000000,
    ABP_PERI_SIZE = 0x00080000,
    POWER_BASE    = 0x40000000,
    CLOCK_BASE    = 0x40000000,
    MPU_BASE      = 0x40000000,
    AMLI_BASE     = 0x40000000,
    RADIO_BASE    = 0x40001000,
    UART0_BASE    = 0x40002000,
    SPI0_BASE     = 0x40003000,
    TWI0_BASE     = 0x40003000,
    SPI1_BASE     = 0x40004000,
    TWI1_BASE     = 0x40004000,
    SPIS1_BASE    = 0x40004000,
    SPIM1_BASE    = 0x40004000,
    GPIOTE_BASE   = 0x40006000,
    ADC_BASE      = 0x40007000,
    TIMER0_BASE   = 0x40008000,
    TIMER1_BASE   = 0x40009000,
    TIMER2_BASE   = 0x4000A000,
    RTC0_BASE     = 0x4000B000,
    TEMP_BASE     = 0x4000C000,
    RNG_BASE      = 0x4000D000,
    ECB_BASE      = 0x4000E000,
    AAR_BASE      = 0x4000F000,
    CCM_BASE      = 0x4000F000,
    WDT_BASE      = 0x40010000,
    RTC1_BASE     = 0x40011000,
    QDEC_BASE     = 0x40012000,
    LPCOMP_BASE   = 0x40013000,
    SWI_BASE      = 0x40014000,
    NVMC_BASE     = 0x4001E000,
    PPI_BASE      = 0x4001F000,
    GPIO_BASE     = 0x50000000,
    FICR_BASE     = 0x10000000,
    UICR_BASE     = 0x10001000,
    LED_BASE      = 0x40020000,
};

static void microbit_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
}

static void microbit_load_kernel(ARMCPU *cpu, const char *kernel_filename,
                                 int mem_size)
{
    int ret;
    ret = load_image_targphys(kernel_filename, STARTUP_ADDR, mem_size);

    if (ret < 0) {
        error_report("%s: Failed to load file %s", __func__,
                     kernel_filename);
        exit(1);
    }

    qemu_register_reset(microbit_cpu_reset, cpu);
}

static void microbit_copy_vector(MemoryRegion *dest_mem, hwaddr src_base,
                                 size_t vec_size)
{
    uint32_t* dest = (uint32_t *)memory_region_get_ram_ptr(dest_mem);

    for(int i = 0; i < vec_size / 4; i++) {
        dest[i] = ldl_p(rom_ptr(src_base + i * 4));
    }
}

typedef enum {
    DEVICE_UNIMPL,
    DEVICE_SIMPLE,
} microbit_device_type_t;

typedef struct {
    const char *           name;
    uint32_t               base_addr;
    uint32_t               size;
    microbit_device_type_t type;
} microbit_device_info_t;

static const microbit_device_info_t microbit_devices[] = {
    {"radio",                 RADIO_BASE,  0x1000, DEVICE_UNIMPL},
    {"uart0",                 UART0_BASE,  0x1000, DEVICE_UNIMPL},
    {"spi0",                  SPI0_BASE,   0x1000, DEVICE_UNIMPL},
    {"twi0",                  TWI0_BASE,   0x1000, DEVICE_UNIMPL},
    {"spi1",                  SPI1_BASE,   0x1000, DEVICE_UNIMPL},
    {"twi1",                  TWI1_BASE,   0x1000, DEVICE_UNIMPL},
    {"spis1",                 SPIS1_BASE,  0x1000, DEVICE_UNIMPL},
    {"spim1",                 SPIM1_BASE,  0x1000, DEVICE_UNIMPL},
    {"gpiote",                GPIOTE_BASE, 0x1000, DEVICE_UNIMPL},
    {"adc",                   ADC_BASE,    0x1000, DEVICE_UNIMPL},
    {"rtc0",                  RTC0_BASE,   0x1000, DEVICE_UNIMPL},
    {"temp",                  TEMP_BASE,   0x1000, DEVICE_UNIMPL},
    {"ecb",                   ECB_BASE,    0x1000, DEVICE_UNIMPL},
    {"aar",                   AAR_BASE,    0x1000, DEVICE_UNIMPL},
    {"ccm",                   CCM_BASE,    0x1000, DEVICE_UNIMPL},
    {"wdt",                   WDT_BASE,    0x1000, DEVICE_UNIMPL},
    {"rtc1",                  RTC1_BASE,   0x1000, DEVICE_UNIMPL},
    {"qdec",                  QDEC_BASE,   0x1000, DEVICE_UNIMPL},
    {"lpcomp",                LPCOMP_BASE, 0x1000, DEVICE_UNIMPL},
    {"swi",                   SWI_BASE,    0x1000, DEVICE_UNIMPL},
    {"ppi",                   PPI_BASE,    0x1000, DEVICE_UNIMPL},
    {"uicr",                  UICR_BASE,   0x1000, DEVICE_UNIMPL},
    {"unknown",               0xF0000000,  0x1000, DEVICE_UNIMPL},
    {"microbit_led_matrix",   LED_BASE,    0x1000, DEVICE_SIMPLE},
    {"nrf51_gpio",            GPIO_BASE,   0x1000, DEVICE_SIMPLE},
    {"nrf51_rng",             RNG_BASE,    0x1000, DEVICE_SIMPLE},
    {"nrf51_nvmc",            NVMC_BASE,   0x1000, DEVICE_SIMPLE},
    {"nrf51_ficr",            FICR_BASE,   0x1000, DEVICE_SIMPLE},
    {"nrf51_clock_power_mpu", CLOCK_BASE,  0x1000, DEVICE_SIMPLE},
};

static void microbit_create_devices(void)
{
    const microbit_device_info_t *dev = microbit_devices;
    for (int i = 0; i < ARRAY_SIZE(microbit_devices); i++) {
        switch (dev->type) {
            case DEVICE_UNIMPL:
                create_unimplemented_device(dev->name, dev->base_addr,
                                            dev->size);
                break;
            case DEVICE_SIMPLE:
                sysbus_create_simple(dev->name, dev->base_addr, NULL);
                break;
            default:
                g_assert_not_reached();
                break;
        }
        dev ++;
    }
}

static void microbit_init(MachineState *machine)
{
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *code_loader = g_new(MemoryRegion, 1);
    DriveInfo *dinfo;
    pflash_t *flash;

    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MICROBITMachineState *mbs = MICROBIT_MACHINE(machine);
    DeviceState *armv7m;

    /* Check configuration */
    if (strcmp(machine->cpu_type, mc->default_cpu_type) != 0) {
        error_report("microbit: This board can only be used with CPU [%s].",
                     mc->default_cpu_type);
        exit(1);
    }
    if ((machine->ram_size != 32 * 1024) && (machine->ram_size != 16 * 1024)) {
        error_report("microbit: RAM size must be 16KB or 32KB");
        exit(1);
    }

    /* Initial architecture */
    object_initialize(&mbs->armv7m, sizeof(mbs->armv7m), TYPE_ARMV7M);
    armv7m = DEVICE(&mbs->armv7m);
    qdev_set_parent_bus(armv7m, sysbus_get_default());
    qdev_prop_set_uint32(armv7m, "num-irq", NUM_IRQ);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    object_property_set_link(OBJECT(armv7m), OBJECT(get_system_memory()),
                                     "memory", &error_abort);
    object_property_set_bool(OBJECT(armv7m), true, "realized",
                             &error_fatal);

    /* RAM */
    memory_region_allocate_system_memory(ram, NULL, "microbit.ram",
                                         machine->ram_size);
    memory_region_add_subregion(get_system_memory(), RAM_BASE, ram);

    /* CODE: ROM */
    memory_region_allocate_system_memory(code_loader, NULL,
                                         "microbit.code_loader",
                                         CODE_LOADER_SIZE);
    memory_region_set_readonly(code_loader, true);
    memory_region_add_subregion(get_system_memory(), CODE_LOADER_BASE,
                                code_loader);

    /* CODE: FLASH */
    dinfo = drive_get(IF_PFLASH, 0, 0);
    flash = pflash_cfi01_register(CODE_KERNEL_BASE, NULL, "microbit.code_kernel",
                                  CODE_KERNEL_SIZE,
                                  dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
                                  FLASH_SEC_SIZE, CODE_KERNEL_SIZE / FLASH_SEC_SIZE,
                                  4, 0x0000, 0x0000, 0x0000, 0x0000, 0);
    if(!flash){
        error_report("%s: Error registering flash memory.\n", __func__);
        exit(1);
    }

    /* Peripherals */
    microbit_create_devices();
    sysbus_create_simple(TYPE_NRF51_TIMER, TIMER0_BASE,
                         qdev_get_gpio_in(armv7m, 8));
    sysbus_create_simple(TYPE_NRF51_TIMER, TIMER1_BASE,
                         qdev_get_gpio_in(armv7m, 9));
    sysbus_create_simple(TYPE_NRF51_TIMER, TIMER2_BASE,
                         qdev_get_gpio_in(armv7m, 10));

    /* Load binary image */
    microbit_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                         CODE_KERNEL_SIZE);
    microbit_copy_vector(code_loader, CODE_KERNEL_BASE, VECTOR_SIZE);
}

static void microbit_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "micro:bit";
    mc->init = microbit_init;
    /* TODO: use m0 instead */
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m3");
    mc->default_ram_size = 32 * 1024;
}

static const TypeInfo microbit_abstract_info = {
    .name = TYPE_MICROBIT_MACHINE,
    .parent = TYPE_MACHINE,
    .abstract = true,
    .instance_size = sizeof(MICROBITMachineState),
    .class_size = sizeof(MICROBITMachineClass),
    .class_init = microbit_class_init,
};

static const TypeInfo microbit_info = {
    .name = MACHINE_TYPE_NAME(TYPE_MICROBIT_MACHINE),
    .parent = TYPE_MICROBIT_MACHINE,
};

static void microbit_machine_init(void)
{
    type_register_static(&microbit_abstract_info);
    type_register_static(&microbit_info);
}

type_init(microbit_machine_init)

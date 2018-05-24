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
    ptimer_state *timer;
    qemu_irq irq;
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
        case 0x020: /* TODO: remove temperory test port */
            printf("%s: counter %d\n", __func__, s->internal_counter);
            return s->internal_counter;
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
                printf("Timer start.\n");
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
            printf("%s: set mode to %d.\n", __func__, s->mode);
            nrf51_timer_recalibrate(s, 1);
            break;
        case NRF51_TIMER_BITMODE:
            s->bitmode = value & 0x3;
            break;
        case NRF51_TIMER_PRESCALER:
            s->prescaler = value & 0xf;
            printf("%s: set prescaler to %d.\n", __func__, s->prescaler);
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
            qemu_irq_raise(s->irq);
        } else {
            qemu_irq_lower(s->irq);
        }
    } else {
        /* Timer mode */
        for (i = 0; i < 4; i++) {
            if (s->inten & (1 << i)) {
                if (s->cc[i] == s->internal_counter) {
                    s->compare[i] ++;
                    qemu_irq_raise(s->irq);
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

static void microbit_copy_vector(MemoryRegion *dest_mem, hwaddr src_base)
{
    uint32_t data;
    uint32_t* dest = (uint32_t *)memory_region_get_ram_ptr(dest_mem);
    for(int i = 0; i < VECTOR_SIZE / 4; i++) {
        data = ldl_p(rom_ptr(src_base + i * 4));
        dest[i] = data;
    }
}

typedef struct {
    const char * name;
    uint32_t base_addr;
    uint32_t size;
} microbit_device_info;

static const microbit_device_info unimp_devices[] = {
    {"radio",  RADIO_BASE,  0x1000},
    {"uart0",  UART0_BASE,  0x1000},
    {"spi0",   SPI0_BASE,   0x1000},
    {"twi0",   TWI0_BASE,   0x1000},
    {"spi1",   SPI1_BASE,   0x1000},
    {"twi1",   TWI1_BASE,   0x1000},
    {"spis1",  SPIS1_BASE,  0x1000},
    {"spim1",  SPIM1_BASE,  0x1000},
    {"gpiote", GPIOTE_BASE, 0x1000},
    {"adc",    ADC_BASE,    0x1000},
    {"rtc0",   RTC0_BASE,   0x1000},
    {"temp",   TEMP_BASE,   0x1000},
    {"ecb",    ECB_BASE,    0x1000},
    {"aar",    AAR_BASE,    0x1000},
    {"ccm",    CCM_BASE,    0x1000},
    {"wdt",    WDT_BASE,    0x1000},
    {"rtc1",   RTC1_BASE,   0x1000},
    {"qdec",   QDEC_BASE,   0x1000},
    {"lpcomp", LPCOMP_BASE, 0x1000},
    {"swi",    SWI_BASE,    0x1000},
    {"ppi",    PPI_BASE,    0x1000},
    {"uicr",   UICR_BASE,   0x1000},
    {"gpio",   GPIO_BASE,   0x1000},
    {"unknown",0xF0000000,  0x1000},
};

static void microbit_create_unimp_devices(void)
{
    int i;
    for (i = 0; i < sizeof(unimp_devices) / sizeof(microbit_device_info); i++) {
        create_unimplemented_device(unimp_devices[i].name,
                                    unimp_devices[i].base_addr,
                                    unimp_devices[i].size);
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
    microbit_create_unimp_devices();
    sysbus_create_simple(TYPE_NRF51_RNG, RNG_BASE, NULL);
    sysbus_create_simple(TYPE_NRF51_NVMC, NVMC_BASE, NULL);
    sysbus_create_simple(TYPE_NRF51_FICR, FICR_BASE, NULL);
    sysbus_create_simple(TYPE_NRF51_CPM, CLOCK_BASE, NULL);
    sysbus_create_simple(TYPE_NRF51_TIMER, TIMER0_BASE,
                         qdev_get_gpio_in(armv7m, 8));
    sysbus_create_simple(TYPE_NRF51_TIMER, TIMER1_BASE,
                         qdev_get_gpio_in(armv7m, 9));
    sysbus_create_simple(TYPE_NRF51_TIMER, TIMER2_BASE,
                         qdev_get_gpio_in(armv7m, 10));

    /* Load binary image */
    microbit_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                         CODE_KERNEL_SIZE);
    microbit_copy_vector(code_loader, CODE_KERNEL_BASE);
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

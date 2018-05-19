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

static uint64_t nrf51_timer_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    NRF51TimerState *s = (NRF51TimerState *)opaque;

    switch (offset) {
        case 0x000:
            return s->start;
        case 0x004:
            return s->stop;
        case 0x008:
            return s->count;
        case 0x00c:
            return s->clear;
        case 0x010:
            return s->shutdown;
        case 0x040:
        case 0x044:
        case 0x048:
        case 0x04c:
            return s->capture[(offset - 0x040) >> 2];
        case 0x140:
        case 0x144:
        case 0x148:
        case 0x14c:
            return s->compare[(offset - 0x140) >> 2];
        case 0x200:
            return s->shorts;
        case 0x304:
        case 0x308:
            qemu_log_mask(LOG_UNIMP,
                          "%s: `INTEN` not implemented when reading 0x%x\n",
                          __func__,
                          (int)offset);
            return 0;
        case 0x504:
            return s->mode;
        case 0x508:
            return s->bitmode;
        case 0x510:
            return s->prescaler;
        case 0x540:
        case 0x544:
        case 0x548:
        case 0x54c:
            return s->cc[(offset - 0x540) >> 2];
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
        case 0x000: /* Start */
            if (value & 1) {
                ptimer_set_freq(s->timer, s->freq);
                if (!s->pulsed)
                    nrf51_timer_recalibrate(s, 1);
                else
                    s->pulsed = false;
                ptimer_run(s->timer, 0);
            }
            break;
        case 0x004: /* Stop */
            if (value & 1) {
                ptimer_stop(s->timer);
                s->pulsed = true;
            }
            break;
        case 0x008: /* Count */
            if (s->bitmode & 1) {
                s->count = value;
                nrf51_timer_recalibrate(s, 1);
            }
            break;
        case 0x00c: /* Clear */
            if (value & 1) {
                s->internal_counter = 0;
                nrf51_timer_recalibrate(s, 1);
            }
            break;
        case 0x010: /* Shutdown */
            if (value & 1) {
                ptimer_stop(s->timer);
                s->internal_counter = 0;
                nrf51_timer_recalibrate(s, 1);
                s->pulsed = false;
            }
            break;
        case 0x040: /* Capture[0:3] */
        case 0x044:
        case 0x048:
        case 0x04c:
            s->cc[(offset >> 2) & 0x3] = s->internal_counter;
            break;
        case 0x140: /* Compare[0:3] */
        case 0x144:
        case 0x148:
        case 0x14c:
            s->compare[(offset >> 2) & 0x3] = value;
            break;
        case 0x200: /* Shorts */
            qemu_log_mask(LOG_UNIMP,
                          "%s: `SHORTS` not implemented when writing 0x%x\n",
                          __func__,
                          (int)offset);
            break;
        case 0x304: /* Intenset */
            s->inten |= (value >> 16) & 0xf;
            break;
        case 0x308:
            /* TODO: verify this formula */
            s->inten &= (~(value >> 16)) & 0xf;
            break;
        case 0x504:
            s->mode = value & 1;
            nrf51_timer_recalibrate(s, 1);
            break;
        case 0x508:
            s->bitmode = value & 0x3;
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
            break;
        case 0x510:
            s->prescaler = value & 0xf;
            nrf51_timer_recalibrate(s, 1);
            break;
        case 0x540:
        case 0x544:
        case 0x548:
        case 0x54c:
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
    
    /* Compare */
    for (i = 0; i < 4; i++) {
        if (s->inten & (1 << i)) {
            if (s->cc[i] == s->internal_counter) {
                s->compare[i] ++;
                qemu_irq_raise(s->irq);
            }
        } else {
            qemu_irq_lower(s->irq);
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
    s->timer = ptimer_init(bh, PTIMER_POLICY_DEFAULT);
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

static void microbit_init(MachineState *machine)
{
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *code = g_new(MemoryRegion, 1);
    MemoryRegion *apb_peri = g_new(MemoryRegion, 1);

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

    object_initialize(&mbs->armv7m, sizeof(mbs->armv7m), TYPE_ARMV7M);
    armv7m = DEVICE(&mbs->armv7m);

    qdev_set_parent_bus(armv7m, sysbus_get_default());
    qdev_prop_set_uint32(armv7m, "num-irq", 64);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    object_property_set_link(OBJECT(armv7m), OBJECT(get_system_memory()),
                                     "memory", &error_abort);
    object_property_set_bool(OBJECT(armv7m), true, "realized",
                             &error_fatal);

    /* RAM: 0x20000000, 16KB - 32KB */
    memory_region_allocate_system_memory(ram, NULL, "microbit.ram",
                                         machine->ram_size);
    memory_region_add_subregion(get_system_memory(), 0x20000000, ram);

    /* CODE: 0x00000000, 128KB or 256KB
     * TODO: use flash instead of ram
     */
    memory_region_allocate_system_memory(code, NULL, "microbit.code",
                                         128 * 1024);
    memory_region_add_subregion(get_system_memory(), 0x00000000, code);

    /**
     * APB Peripheral: 0x40000000 - 0x40080000
     *     POWER    0x40000000
     *     CLOCK    0x40000000
     *     MPU      0x40000000
     *     AMLI     0x40000000
     *     RADIO    0x40001000
     *     UART0    0x40002000
     *     SPI0     0x40003000
     *     TWI0     0x40003000
     *     SPI1     0x40004000
     *     TWI1     0x40004000
     *     SPIS1    0x40004000
     *     SPIM1    0x40004000
     *     GPIOTE   0x40006000
     *     ADC      0x40007000
     *     TIMER0   0x40008000
     *     TIMER1   0x40009000
     *     TIMER2   0x4000A000
     *     RTC0     0x4000B000
     *     TEMP     0x4000C000
     *     RNG      0x4000D000
     *     ECB      0x4000E000
     *     AAR      0x4000F000
     *     CCM      0x4000F000
     *     WDT      0x40010000
     *     RTC1     0x40011000
     *     QDEC     0x40012000
     *     LPCOMP   0x40013000
     *     SWI      0x40014000
     *     NVMC     0x4001E000
     *     PPI      0x4001F000
     */
    memory_region_init(apb_peri, NULL, "microbit.apb_peripheral", 0x00080000);
    memory_region_add_subregion(get_system_memory(), 0x40000000, apb_peri);
    create_unimplemented_device("pwr_clk_mpu", 0x40000000, 0x1000);
    sysbus_create_simple(TYPE_NRF51_TIMER, 0x40008000, qdev_get_gpio_in(armv7m, 8));
    sysbus_create_simple(TYPE_NRF51_TIMER, 0x40009000, qdev_get_gpio_in(armv7m, 9));
    sysbus_create_simple(TYPE_NRF51_TIMER, 0x4000A000, qdev_get_gpio_in(armv7m, 10));

    armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename, 128 * 1024);
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

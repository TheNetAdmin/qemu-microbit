#include "hw/arm/microbit.h"
#include "cpu.h"
#include "hw/arm/arm.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ptimer.h"

/**
 * NRF51 Internal Timer
 *   Not registered as a device
 *   Takes up 0x1000 bytes space
 */

#define NRF51_TIMER_BASE_FREQ 0x01000000

typedef struct {
    /* Internal timer */
    ptimer_state *timer;
    qemu_irq irq;
    uint32_t int_level;
    uint32_t timer_id;

    /* TODO: control? */

    /**
     * freq = 16MHz / (2 ^ prescaler)
     * 0 <= prescaler <= 9
     * i.e:
     *   16MHz  <= freq <= 32KHz
     *   62.5ns <= tick <= 31.25us
     */
    uint32_t freq;

    /* Regs */
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
} nrf51_sub_timer_state;

static uint64_t nrf51_sub_timer_read(void *opaque, hwaddr offset)
{
    nrf51_sub_timer_state *s = (nrf51_sub_timer_state *)opaque;

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
    }
}

static void nrf51_sub_timer_write(void *opaque, hwaddr offset,
                              uint64_t value)
{
    nrf51_sub_timer_state *s = (nrf51_sub_timer_state *)opaque;

    switch (offset) {
        /* TODO: finish */
    };
}

static const VMStateDescription vmstate_nrf51_sub_timer = {
    .name = "nrf51_sub_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    /* TODO: update */
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(start,         nrf51_timer_state),
        VMSTATE_UINT32(stop,          nrf51_timer_state),
        VMSTATE_UINT32(count,         nrf51_timer_state),
        VMSTATE_UINT32(clear,         nrf51_timer_state),
        VMSTATE_UINT32(shutdown,      nrf51_timer_state),
        VMSTATE_UINT32_ARRAY(capture, nrf51_timer_state, 4),
        VMSTATE_UINT32_ARRAY(compare, nrf51_timer_state, 4),
        VMSTATE_UINT32(shorts,        nrf51_timer_state),
        VMSTATE_UINT32(intenset,      nrf51_timer_state),
        VMSTATE_UINT32(intenclr,      nrf51_timer_state),
        VMSTATE_UINT32(mode,          nrf51_timer_state),
        VMSTATE_UINT32(bitmode,       nrf51_timer_state),
        VMSTATE_UINT32(prescaler,     nrf51_timer_state),
        VMSTATE_UINT32_ARRAY(cc,      nrf51_timer_state, 4),
        VMSTATE_END_OF_LIST()
    }
}

static void nrf51_sub_timer_tick(void *opaque)
{
    nrf51_timer_state *s = (nrf51_timer_state *)opaque;
    /* TODO: finish */
}

static nrf51_sub_timer_state *nrf51_sub_timer_new(uint32_t id)
{
    QEMUBH *bh;

    s = g_new0(nrf51_sub_timer_state, 1);
    s->timer_id = id;
    // TODO: when to set int_level?
    // s->int_level = level;

    bh = qemu_bh_new(nrf51_sub_timer_tick, s);
    s->timer = ptimer_init(bh, PTIMER_POLICY_DEFAULT);
    vmstate_register(NULL, -1, &vmstate_nrf51_sub_timer, s);

    return s;
}

/**
 * NRF51 Timer
 *   With respect to nRF51822 Reference Manual
 *   contains 3 internal timers
 */

#define TYPE_NRF51_TIMER "nrf51-timer"
#define NRF51_TIMER(obj) \
    OBJECT_CHECK(nrf51_timer_state, (obj), TYPE_NRF51_TIMER)

typedef struct {
    /* Private */
    SysBusDevice parent;

    /* Public */
    MemoryRegion iomem;
    nrf51_sub_timer_state *timer[3];
    int level[3];
    qemu_irq irq;
} nrf51_timer_state;

static void nrf51_timer_set_irq(void *opaque, int irq, int level)
{
    nrf51_timer_state *s = (nrf51_timer_state *)opaque;

    s->level[irq] = level;
    qemu_set_irq(s->irq, s->level[0] || s->level[1] || s->level[2]);
}

static uint64_t nrf51_timer_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    nrf51_timer_state *s = (nrf51_sub_timer_state *)opaque;

    if(offset >= 3 * 0x1000) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: reading a bad offset 0x%x\n",
                      TYPE_NRF51_TIMER,
                      (int)offset);
        return 0;
    }

    return nrf51_sub_timer_read(s->timer[offset >> 12],
                                offset & 0xfff);
}

static uint64_t nrf51_timer_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    nrf51_timer_state *s = (nrf51_sub_timer_state *)opaque;

    if(offset >= 3 * 0x1000) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: writeing a bad offset 0x%x\n",
                      TYPE_NRF51_TIMER,
                      (int)offset);
        return 0;
    }

    return nrf51_sub_timer_write(s->timer[offset >> 12],
                                 offset & 0xfff,
                                 value);
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
        VMSTATE_END_OF_LIST()
    }
};

static void nrf51_timer_init(Object *obj)
{
    nrf51_timer_state *s = NRF51_TIMER(obj);
    SysBusDevice *sdb = SYS_BUS_DEVICE(obj);

    sysbus_init_irq(sdb, &s->irq);
    memory_region_init_io(&s->iomem, obj, &nrf51_timer_ops, s,
                          TYPE_NRF51_TIMER, 0x3000);
    sysbus_init_mmio(sdb, &s->iomem);
}

static void nrf51_timer_realize(DeviceState *dev, Error **errp)
{
    nrf51_timer_state *s = NRF51_TIMER(dev);

    /* TODO: irq level 8, 9, 10 */
    s->timer[0] = nrf51_sub_timer_new(0);
    s->timer[1] = nrf51_sub_timer_new(1);
    s->timer[2] = nrf51_sub_timer_new(2);
}

/**
 * micro:bit machine type
 */
static void microbit_init(MachineState *machine)
{
    ARMCPU *cpu;
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *code = g_new(MemoryRegion, 1);
    MemoryRegion *apb_peri = g_new(MemoryRegion, 1);
    MemoryRegion *ahb_peri = g_new(MemoryRegion, 1);
    MachineClass *mc = MACHINE_GET_CLASS(machine);

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

    cpu = ARM_CPU(object_new(machine->cpu_type));

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

    /* APB Peripheral: 0x40000000 - 0x40080000 */
    memory_region_init(apb_peri, NULL, "microbit.apb_peripheral", 0x00080000);
    memory_region_add_subregion(get_system_memory(), 0x40000000, apb_peri);
}

static void microbit_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "micro:bit";
    mc->init = microbit_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m0");
    mc->default_ram_size = 32 * 1024;
}

static const TypeInfo microbit_info = {
    .name = TYPE_MICROBIT_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = microbit_class_init,
}

static void microbit_machine_init(void)
{
    type_register_static(&microbit_info);
}

type_init(microbit_machine_init);

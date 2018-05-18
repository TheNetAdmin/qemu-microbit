#ifndef HW_ARM_MICROBIT_H
#define HW_ARM_MICROBIT_H

#include "hw/boards.h"

#define TYPE_MICROBIT_MACHINE "micro:bit"

typedef struct {
    /* Private */
    MachineState parent;

    /* Public */
    ARMv7MState armv7m;

} MICROBIT_MachineState;

typedef struct {
    /* Private */
    MachineClass parent;

    /* Public */
} MICROBIT_MachineClass;

#define MICROBIT_MACHINE(obj) \
    OBJECT_CHECK(MICROBIT_MachineState, (obj), TYPE_MICROBIT_MACHINE)

#define MICROBIT_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(MICROBIT_MachineClass, (obj), TYPE_MICROBIT_MACHINE)

#define MICROBIT_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(MICROBIT_MachineClass, (klass), TYPE_MICROBIT_MACHINE)

#endif /* HW_ARM_MICROBIT_H */
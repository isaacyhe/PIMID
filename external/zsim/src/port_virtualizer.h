/* Stub PortVirtualizer -- the Pin-era port virtualization layer is unused in
 * QEMU builds, but GlobSimInfo still carries the portVirt[] array and init.cpp
 * allocates the objects.  Provide a minimal definition so the code compiles. */

#ifndef PORT_VIRTUALIZER_H_
#define PORT_VIRTUALIZER_H_

class PortVirtualizer {
public:
    PortVirtualizer() {}
};

#endif // PORT_VIRTUALIZER_H_

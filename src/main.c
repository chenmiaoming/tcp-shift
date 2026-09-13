#include <stdio.h>

#include "lwip/init.h"

int main(void)
{
    lwip_init();
    printf("tcp-shift: lwIP %s initialized (P0)\n", LWIP_VERSION_STRING);
    return 0;
}

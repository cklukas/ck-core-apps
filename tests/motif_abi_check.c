#include <stdio.h>

#include <Xm/Xm.h>

#if !defined(XmABI_VERSION) || XmABI_VERSION != 9
#error "ck-core-apps requires CK Core Motif ABI 9 headers"
#endif

int
main(void)
{
    unsigned int runtime_abi = XmToolkitABI();

    if (runtime_abi != XmABI_VERSION) {
        fprintf(stderr,
                "CK Core Motif header/runtime ABI mismatch: headers=%u runtime=%u\n",
                (unsigned int)XmABI_VERSION, runtime_abi);
        return 1;
    }

    printf("CK Core Motif ABI %u\n", runtime_abi);
    return 0;
}

// SPDX-License-Identifier: MIT
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_backend.h>

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
#define INIT_SYMBOL "__vaDriverInit_" STRINGIFY(VA_MAJOR_VERSION) "_" \
                    STRINGIFY(VA_MINOR_VERSION)

int main(int argc, char **argv)
{
    struct VADriverContext context;
    struct VADriverVTable vtable;
    VADriverInit init;
    VAProfile profiles[1];
    int num_profiles = -1;
    void *module;

    if (argc != 2) {
        fprintf(stderr, "usage: %s DRIVER\n", argv[0]);
        return 2;
    }

    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    *(void **)(&init) = dlsym(module, INIT_SYMBOL);
    if (!init) {
        fprintf(stderr, "missing %s: %s\n", INIT_SYMBOL, dlerror());
        dlclose(module);
        return 1;
    }

    memset(&context, 0, sizeof(context));
    memset(&vtable, 0, sizeof(vtable));
    context.vtable = &vtable;

    if (setenv("VENUS_VAAPI_ALLOW_NO_DEVICE", "1", 1) != 0 ||
        setenv("VENUS_VAAPI_FORCE_NO_DEVICE", "1", 1) != 0 ||
        init(&context) != VA_STATUS_SUCCESS ||
        !context.pDriverData ||
        !vtable.vaQueryConfigProfiles ||
        vtable.vaQueryConfigProfiles(&context, profiles, &num_profiles) !=
            VA_STATUS_SUCCESS ||
        num_profiles != 0 ||
        !vtable.vaTerminate ||
        vtable.vaTerminate(&context) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "driver initialization contract failed\n");
        dlclose(module);
        return 1;
    }

    dlclose(module);
    puts("PASS: driver loaded and initialized through dlopen");
    return 0;
}

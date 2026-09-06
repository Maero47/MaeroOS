#pragma once

#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) \
            panic("Assertion failed: " msg " at " __FILE__ ":" STRINGIFY(__LINE__), NULL); \
    } while (0)

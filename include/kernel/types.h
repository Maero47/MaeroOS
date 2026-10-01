#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;

typedef uintptr_t physaddr_t;
typedef uintptr_t virtaddr_t;
typedef uint64_t  pte_t;   /* also arch/i686/mm/paging.h: PAE entries are 64-bit */
typedef uint64_t  pde_t;

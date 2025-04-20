#include "allocator.h"

unsigned sys_page_size = 0;

Allocator default_allocator = {};

[[ gnu::constructor ]]
static void init_page_size()
{
    if (sys_page_size == 0) {
        sys_page_size = sysconf(_SC_PAGE_SIZE);
    }
}

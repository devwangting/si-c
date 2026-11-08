#include <stdio.h>
#include "util.h"

#define GREETING "hello"

typedef struct Point {
    int x;
    int y;
} Point;

static int helper(int value)
{
    return value + 1;
}

int main(void)
{
    Point p = {1, 2};
    printf("%s %d\n", GREETING, helper(p.x));
    util_global++; return add(p.x, p.y);
}

int global_counter = 0;
int shadow_value = 1;

int shadow_test(void)
{
    int shadow_value = 2;
    return shadow_value;
}

static int usage(int x)




{
    return x + 2;
}

int use_local_usage(void)
{
    return usage(1);
}


#include "util.h"

int util_global = 7;

int add(int a, int b)
{
    return a + b;
}

int sub(int a, int b)
{
    return a - b;
}

int top(int x)
{
    return add(x, 1);
}

int caller(int x)
{
    return top(x);
}

static int usage(int x)
{
    return x + 3;
}

int use_util_usage(void)
{
    return usage(2);
}

int global_counter = 9;

static UtilBox shared_point;

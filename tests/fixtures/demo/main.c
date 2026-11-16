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

typedef struct CompletionNode {
    Point pos;
    struct CompletionNode *next;
    int alpha, beta;
} CompletionNode;

typedef struct {
    int alpha, beta;
} CompletionAnon;

struct CompletionForward;

struct Point completion_origin;
Point completion_world = {1, 2};
struct Point *completion_ptr;
int completion_a, completion_b;
struct Point completion_p, *completion_q;
int completion_arr[4];

int completion_demo(void)
{
    int result = 0, other;
    Point local_p, *local_q;
    CompletionNode node;
    node.pos.x = result + other + local_p.x + local_q->x;
    return node.pos.y + completion_origin.x + completion_world.y +
           completion_ptr->x;
}


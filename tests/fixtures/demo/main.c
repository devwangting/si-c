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
    Point point_arr[2];
} CompletionNode;

typedef struct {
    int alpha, beta;
} CompletionAnon;

struct CompletionForward;

struct Point completion_origin;
Point completion_world = {1, 2};
struct Point *completion_ptr;
struct Point completion_points[3];
static struct Point shared_point;
int completion_a, completion_b;
struct Point completion_p, *completion_q;
int completion_arr[4];

int completion_demo(void)
{
    int result = 0, other;
    Point local_p, *local_q;
    Point local_arr[2];
    CompletionNode node;
#define COMPLETION_LOCAL_MACRO(x) \
    x + result
    node.pos.x = result + other + local_p.x + local_q->x;
    return node.pos.y + completion_origin.x + completion_world.y +
           completion_ptr->x;
}

enum class CompletionMode;
enum class CompletionMode2 : unsigned;

static int callback_target(int x)
{
    return x;
}

int callback_register_demo(void)
{
    void (*fn)(int) = callback_target;
    (void)fn;
    return 0;
}

int callback_signal_demo(void)
{
    signal(SIGINT, callback_target);
    return 0;
}

#ifdef BRANCH_A
static int branch_alt(void) {
#else
static int branch_main(void) {
#endif
    if (callback_target(1))
        return 1;
    return 0;
}

class BaseThing {
public:
    int base_value;
    void base_method();
};

class Thing : public BaseThing {
public:
    int value;
    static int count;
    static void static_method();
    void method();
    struct Inner { int z; static int inner_count; void inner_method(); };
    int use_this() { return this->value; }
};

namespace demo_ns {
class NsThing {
public:
    int ns_value;
    static int ns_count;
    void ns_method();
};
}

static void Thing::static_method() {}
void Thing::method() {}
int Thing::count = 0;
void demo_ns::NsThing::ns_method() {}

class AlphaThing {
public:
    void ping();
    void call();
};

class BetaThing {
public:
    void ping();
    void call();
};

void AlphaThing::ping() {}
void BetaThing::ping() {}
void AlphaThing::call() { ping(); }
void BetaThing::call() { ping(); }

int thing_demo(void)
{
    Thing thing;
    thing.value = 1;
    return thing.use_this();
}


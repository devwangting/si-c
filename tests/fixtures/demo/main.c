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
    int read_value() {
        return value;
    }
    Thing make_thing();
    Thing *make_ptr();
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
Thing Thing::make_thing() { return Thing(); }
Thing *Thing::make_ptr() { return 0; }
Thing make_global_thing(void);
Thing make_global_thing(void) { return Thing(); }
void demo_ns::NsThing::ns_method() {}

using AliasThing = Thing;

int alias_demo(void)
{
    AliasThing alias;
    alias.value = 2;
    return alias.value;
}

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

int auto_demo(void)
{
    auto made = Thing();
    auto copy = made;
    auto *ptr = &made;
    auto &ref = made;
    auto made2 = made.make_thing();
    auto *made3 = made.make_ptr();
    auto made4 = make_global_thing();
    made.value = 2;
    made2.value = 3;
    made3->value = 4;
    made4.value = 5;
    return ptr->value + ref.value + copy.value + made2.value +
           made3->value + made4.value;
}

template <typename T>
class Box {
public:
    T item;
    void put(T v);
    static int box_count;
};

int template_demo(void)
{
    Box<int> box;
    box.item = 1;
    return box.item;
}

int constexpr_demo(void)
{
    constexpr Thing cthing{};
    cthing.value = 6;
    return cthing.value;
}


#ifndef DEMO_UTIL_H
#define DEMO_UTIL_H

struct Point;

int add(int a, int b);
int sub(int a, int b);
int top(int x);
int caller(int x);
extern int util_global;

typedef struct UtilBox {
    int w;
} UtilBox;

#endif

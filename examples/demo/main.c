#include <stdio.h>
#include <stdlib.h>

#include "vec.h"

#define VERSION "1.0"
#define SQUARE(x) ((x) * (x))

static void usage(const char *argv0)
{
    printf("usage: %s [count]\n", argv0);
}

int main(int argc, char **argv)
{
    Vec *v;
    int count = 8;
    int i;

    if (argc > 1)
        count = atoi(argv[1]);
    if (count <= 0) {
        usage(argv[0]);
        return 1;
    }

    v = vec_new();
    if (!v)
        return 1;
    for (i = 0; i < count; i++) {
        if (vec_push(v, SQUARE(i)) != VEC_OK) {
            vec_free(v);
            return 1;
        }
    }
    printf("si-c demo %s: ", VERSION);
    for (i = 0; i < v->len; i++)
        printf("%d ", v->data[i]);
    printf("\n");
    vec_free(v);
    return 0;
}

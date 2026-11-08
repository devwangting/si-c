#include "vec.h"

#include <stdlib.h>

#define INITIAL_CAP 4
#define GROW_CAP(cap) ((cap) < 8 ? 8 : (cap) * 2)

Vec *vec_new(void)
{
    Vec *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->data = malloc(INITIAL_CAP * sizeof(*v->data));
    if (!v->data) {
        free(v);
        return NULL;
    }
    v->cap = INITIAL_CAP;
    return v;
}

void vec_free(Vec *v)
{
    if (!v)
        return;
    free(v->data);
    free(v);
}

int vec_push(Vec *v, int value)
{
    if (!v)
        return VEC_RANGE;
    if (v->len == v->cap) {
        int next = GROW_CAP(v->cap);
        int *grown = realloc(v->data, (size_t)next * sizeof(*grown));
        if (!grown)
            return VEC_NOMEM;
        v->data = grown;
        v->cap = next;
    }
    v->data[v->len++] = value;
    return VEC_OK;
}

int vec_get(const Vec *v, int index, int *out)
{
    if (!v || !out || index < 0 || index >= v->len)
        return VEC_RANGE;
    *out = v->data[index];
    return VEC_OK;
}

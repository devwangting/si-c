#ifndef DEMO_VEC_H
#define DEMO_VEC_H

typedef struct Vec {
    int *data;
    int len;
    int cap;
} Vec;

typedef enum VecStatus {
    VEC_OK = 0,
    VEC_NOMEM,
    VEC_RANGE
} VecStatus;

Vec *vec_new(void);
void vec_free(Vec *v);
int vec_push(Vec *v, int value);
int vec_get(const Vec *v, int index, int *out);

#endif

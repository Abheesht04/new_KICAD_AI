#include <assert.h>
#include <stdio.h>

#include "vec.h"

typedef struct {
    int id;
    double value;
} TestObject;

int main(void)
{
    int *values = NULL;

    assert(vec_len(values) == 0);
    assert(vec_cap(values) == 0);

    for (int i = 0; i < 1000; ++i) {
        int value = i * 10;
        assert(vec_push(values, value) == 0);
    }

    assert(vec_len(values) == 1000);
    assert(vec_cap(values) >= 1000);

    for (size_t i = 0; i < vec_len(values); ++i) {
        assert(values[i] == (int)(i * 10));
    }

    assert(vec_reserve(values, 5000) == 0);
    assert(vec_len(values) == 1000);
    assert(vec_cap(values) >= 5000);

    for (size_t i = 0; i < vec_len(values); ++i) {
        assert(values[i] == (int)(i * 10));
    }

    size_t capacity = vec_cap(values);
    vec_clear(values);
    assert(vec_len(values) == 0);
    assert(vec_cap(values) == capacity);

    vec_free(values);
    assert(values == NULL);

    TestObject *objects = NULL;

    for (int i = 0; i < 100; ++i) {
        TestObject object = {i, i * 0.5};
        assert(vec_push(objects, object) == 0);
    }

    assert(vec_len(objects) == 100);

    for (size_t i = 0; i < vec_len(objects); ++i) {
        assert(objects[i].id == (int)i);
        assert(objects[i].value == (double)i * 0.5);
    }

    vec_free(objects);

    printf("Vec test PASSED\n");
    return 0;
}

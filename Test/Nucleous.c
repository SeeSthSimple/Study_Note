#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int *data;
    size_t size;
} IntBuffer;

static int buffer_init(IntBuffer *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return -1;
    }

    buf->data = calloc(size, sizeof(int));
    if (buf->data == NULL) {
        buf->size = 0;
        return -1;
    }

    buf->size = size;
    return 0;
}

static int buffer_max(const IntBuffer *buf, int *out_max)
{
    int max ;
    //初始化检查
    if (buf == NULL || buf->data == NULL || out_max == NULL) {
        return -1;
    }

    max = buf->data[0];
    for (size_t i = 1; i < buf->size; i++)
    {
        if (buf->data[i] > max)
        {
            /* code */
            max = buf->data[i];
        }
        
    }
    

    *out_max = max;
    return 0;
}

static void buffer_destroy(IntBuffer *buf)
{
    if (buf == NULL) {
        return;
    }

    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
}

int main(void)
{
    IntBuffer buf = {0};
    int max = 0;
    size_t i;

    if (buffer_init(&buf, 4) != 0) {
        fprintf(stderr, "buffer_init failed\n");
        return 1;
    }

    for (i = 0; i < buf.size; ++i) {
        buf.data[i] = (int)(i + 1);
    }

    if (buffer_max(&buf, &max) != 0) {
        fprintf(stderr, "buffer_max failed\n");
        buffer_destroy(&buf);
        return 1;
    }

    printf("max = %d\n", max);
    buffer_destroy(&buf);
    return 0;
}
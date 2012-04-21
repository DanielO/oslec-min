#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "fifo.h"

int
fifo_dump(struct fifo *f) {
    int		i;
    uint8_t	tmp;
    
    i = 0;
    while (fifo_get(f, &tmp, 1) == 1) {
	i++;
	printf("Read %d\n", tmp);
    }

    return i;
}

int
main(int argc, char **argv) {
    struct fifo	*f;
    uint8_t	i, count, d[5], d2[6];
    
    assert((f = fifo_alloc(8)) != NULL);


    for (count = 0; fifo_put(f, &count, 1) == 1; count++)
	;
    
    printf("Wrote %d items into the FIFO\n", count);
    fifo_dump(f);

    for (i = 0; i < sizeof(d); i++)
	d[i] = i;
    
    printf("Wrote %d bytes [0..4]\n", fifo_put(f, d, sizeof(d)));
    printf("Wrote %d bytes [0..2]\n", fifo_put(f, d, sizeof(d)));
    printf("Read %d bytes\n", fifo_get(f, d2, sizeof(d2)));
    for (i = 0; i < sizeof(d2); i++)
	printf("Got %d\n", d2[i]);
    
    printf("Unread %d bytes\n", fifo_unget(f, d2, sizeof(d2)));
    fifo_dump(f);
    
    exit(0);
}

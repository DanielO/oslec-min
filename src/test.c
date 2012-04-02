#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAN_DECLARE(x)	x

#include "spandsp/echo.h"

int
main(int argc, char **argv) {
	echo_can_state_t	*ecst;

	ecst = echo_can_init(256, 0);
}


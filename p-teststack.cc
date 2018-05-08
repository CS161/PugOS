#include "p-lib.hh"

uintptr_t stack_base;
uintptr_t stack_lowest = -1;

void recurse(unsigned n) {
	if (n <= 0) {
		return;
	}

	char stackmem[64];
	stackmem[42] = 'h';
	console_printf("%%rsp: top:%p current:%p diff:%db (n=%d)\n",
		stack_base, read_rsp(), stack_base - read_rsp(), n);
	stack_lowest = MIN(stack_lowest, read_rsp());
	recurse(n-1);

	// does c++ optimize for tail recursion?
	stackmem[30] = 't';
}


void process_main() {
	sys_kdisplay(KDISPLAY_NONE);

	stack_base = ROUNDUP(read_rsp(), PAGESIZE);

	recurse(1000);
	console_printf("Successfully mapped %d extra stack pages\n",
		ROUNDUP(stack_base - stack_lowest, PAGESIZE) / PAGESIZE - 1);
	
	sys_exit(0);
}

#ifndef _RUN_ACTION_SERVER_H
#define _RUN_ACTION_SERVER_H

extern int run_action_server(struct async *as,
	struct sdirs *sdirs, const char *incexc,
	int srestore, int *timer_ret, struct conf *cconf);

#endif

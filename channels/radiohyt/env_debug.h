#ifndef AICS_RADIOHYT_ENV_DEBUG_H
#define AICS_RADIOHYT_ENV_DEBUG_H

#define ENV_ASSERT(x) ast_assert(x)

#if defined AST_DEBUG

#include <assert.h>
#include <stdio.h>

#define ENV_DVAR(x) x

//#define ENV_ASSERT(x) if (!(x)) { int *p = 0; printf("assert(%s)\n", ""#x); fflush(stdout); printf("p=%d", p[0]);}

/*
 * Check if current context belong to specified thread
 */
typedef struct thread_checker
{
    pthread_t thrid;
    int running;

}   Thread_checker;

#define ENV_THREAD_CHECKER(context_name) \
    Thread_checker thread_checker_##context_name

#define ENV_THREAD_INIT_CHECKER(context_name) \
    this->thread_checker_##context_name.thrid = 0;\
    this->thread_checker_##context_name.running = 1

#define ENV_THREAD_CHECK(context_name) \
	do {\
		if (!this->thread_checker_##context_name.running)\
			break;\
		if (this->thread_checker_##context_name.thrid == 0) {\
			this->thread_checker_##context_name.thrid = pthread_self();\
			break;\
		}\
		ENV_ASSERT(this->thread_checker_##context_name.thrid == pthread_self());\
	} while(0)

#define ENV_THREAD_RESET_CHECKER(context_name) \
    this->thread_checker_##context_name.thrid = 0;

#define ENV_THREAD_STOP_CHECKER(context_name) \
    this->thread_checker_##context_name.running = 0;

#define ENV_COMPILE_ASSERT(condition, name)                \
    static const char name[(condition)==0 ? 0 : 1] = { 1 }

#else

#define ENV_DVAR(x)
//#define ENV_ASSERT(x) do {} while (0)
#define ENV_THREAD_CHECKER(context_name)
#define ENV_THREAD_INIT_CHECKER(context_name)
#define ENV_THREAD_CHECK(context_name) do {} while (0)
#define ENV_THREAD_RESET_CHECKER(context_name) do {} while (0)
#define ENV_THREAD_STOP_CHECKER(context_name) do {} while (0)
#define ENV_COMPILE_ASSERT(condition, name)

#endif

#endif


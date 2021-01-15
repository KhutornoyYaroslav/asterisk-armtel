#include "asterisk.h"

#include "asterisk/io.h"
#include "asterisk/select.h"
#include "asterisk/lock.h"
#include "asterisk/sem.h"
#include "asterisk/utils.h"
#include "asterisk/logger.h"
#include "asterisk/sched.h"
#include "asterisk/test.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "env_debug.h"
#include "env_error.h"
#include "monitor.h"

#define RADIOHYT_IO_TIMEOUT_MS (20)

/*
 * POSIX pipe interface
 */
static int  create_pipe(int *reader, int *writer);

static void pipe_init(int *fd);
static int  pipe_read(int fd, void *buf, unsigned *len);
static int  pipe_write(int fd, const void *buf, unsigned *len);
static void pipe_close(int *fd);

/*
 * ���� ������ ��������
 */
typedef enum command_e {
	CMD_ADD,
	CMD_CLOSE,
	CMD_ABORT,
	CMD_USER_PROC,
} Command;

struct async_command_s {
	Command command;

	Transport *tport;

	/* capability to call user function on transport manager's thread */
	int (*user_func)(void *);
	void *user_ctx;

	Incoming_call_callback user_cb;

	struct ast_sem sem;
	int rc;

	AST_LIST_ENTRY(async_command_s) entry;
};

typedef struct async_command_s Async_command;

struct monitor_s
{
	struct io_context *ioc;			/* i/o context */
	int *read_io_id;			/* callback id returned by ast_io_add function */

	struct ast_sched_context *sched_ctx;	/* sheduler context */

	pthread_t thread;			/* own thread */
	ast_mutex_t mutex;			/* own thread's critical section */

	volatile int running;			/* thread procedure has started */
	volatile int stopped;			/* thread procedure has stopped */

	int sleep_pipe;				/* own readable pipe */
	int wakeup_pipe;			/* own writable pipe */

	AST_LIST_HEAD_NOLOCK(, async_command_s) cmd_list;
	AST_LIST_HEAD_NOLOCK(,     transport_s) tport_list;

	ENV_THREAD_CHECKER(monitor_thread);
};

typedef struct monitor_s Monitor;

static Monitor *monitor = NULL;
AST_MUTEX_DEFINE_STATIC(monlock);

#ifdef TEST_FRAMEWORK

AST_TEST_DEFINE(test_monitor);

static int test_th_ready_read(Transport *tport);

static Transport test_tport_r = {
	.fd = -1,
	.io_id = NULL,

	.th_ready_read = test_th_ready_read,
	.th_fin_received = NULL,
	.th_error = NULL,
};

static Transport test_tport_w = {
	.fd = -1,
	.io_id = NULL,

	.th_ready_read = test_th_ready_read,
	.th_fin_received = NULL,
	.th_error = NULL,
};

static int test_pipe_r;
static int test_pipe_w;

static char *test_string = "Hello, world\n";

int test_th_ready_read(Transport *tport)
{
	int rc = E_FAIL;
	unsigned len = 128;
	char data[len];

	if (tport->fd == test_pipe_r) {
		rc = pipe_read(test_pipe_r, data, &len);
	} else if (tport->fd == test_pipe_w) {
		rc = pipe_read(test_pipe_w, data, &len);
	} else {
		ENV_ASSERT(0);
	}

	if (rc == E_OK) {
		ast_log(LOG_WARNING, "recv: %d bytes:\t%s\n", len, data);
	}

	return TPORT_RC_CONTINUE;
}

#endif

/*
 * ����������� ������� ��������
 */
static void monitor_init(Monitor *this);
/* ������� �������, ���������� �� ��������� ���� */
static void *monitor_proc(void *arg);
static int  handle_command(Monitor *this, Async_command *cmd);
static int  run_command(Monitor *this, Async_command *cmd);
static void wakeup(Monitor *this);
static int  io_cmd_callback(int *id, int fd, short events, void *cbdata);
static int  io_tport_callback(int *id, int fd, short events, void *cbdata);

/*
 * ���������� ������� ��������
 */
void monitor_init(Monitor *this)
{
	this->ioc = NULL;
	this->running = 0;
	this->stopped = 0;
	this->read_io_id = NULL;

	this->thread = AST_PTHREADT_NULL;
	ast_mutex_init(&this->mutex);

	pipe_init(&this->sleep_pipe);
	pipe_init(&this->wakeup_pipe);

	AST_LIST_HEAD_INIT_NOLOCK(&this->cmd_list);
	AST_LIST_HEAD_INIT_NOLOCK(&this->tport_list);
}

int monitor_start(void)
{
	int rc;

	ast_mutex_lock(&monlock);

	if (monitor) {
		ast_mutex_unlock(&monlock);
		return E_ALREADY;
	}

	monitor = (Monitor *)malloc(sizeof(Monitor));
	monitor_init(monitor);

	rc = create_pipe(&monitor->sleep_pipe, &monitor->wakeup_pipe);
	if (rc) {
		return rc;
	}
	monitor->running = 1;

	rc = pthread_create(&monitor->thread, NULL, monitor_proc, monitor);
	if (rc) {
		monitor->running = 0;
	}

#ifdef TEST_FRAMEWORK
	AST_TEST_REGISTER(test_monitor);
#endif

	ast_mutex_unlock(&monlock);

	return rc;
}

void monitor_stop(void)
{
	ast_mutex_lock(&monlock);

#ifdef TEST_FRAMEWORK
	AST_TEST_UNREGISTER(test_monitor);
#endif

	if (!monitor) {
		ast_mutex_unlock(&monlock);
		return;
	}

	if (monitor->running && !monitor->stopped) {
		monitor->running = 0;
		wakeup(monitor);
	}
	pthread_join(monitor->thread, NULL);

	free(monitor);
	monitor = NULL;

	ast_mutex_unlock(&monlock);
}

#ifdef TEST_FRAMEWORK

/*
 * Parameters:
    struct ast_test_info *info
    enum ast_test_command cmd
    struct ast_test *test
 */
AST_TEST_DEFINE(test_monitor)
{
	int rc;
	unsigned len, wlen;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_radiohyt_monitor";
		info->category = "/channels/radiohyt/monitor";
		info->summary = "Test of functionality of monitor";
		info->description = "This demonstrates how to use RADIOHYT's monitor";
		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	pipe_init(&test_pipe_r);
	pipe_init(&test_pipe_w);
	rc = create_pipe(&test_pipe_r, &test_pipe_w);
	if (rc) {
		goto end;
	}
	test_tport_r.fd = test_pipe_r;
	test_tport_w.fd = test_pipe_w;

	usleep(1);
	rc = monitor_append_tport(&test_tport_r);
	if (rc) goto end;
	rc = monitor_append_tport(&test_tport_w);
	if (rc) goto end;
	ast_log(LOG_WARNING, "RADIOHYT test transports are added\n");

	len = wlen = strlen(test_string);
	rc = pipe_write(test_pipe_w, test_string, &wlen);
	if (rc || len != wlen) {
		ENV_ASSERT(0);
	}

	usleep(1);
	monitor_remove_tport(&test_tport_w);
	monitor_remove_tport(&test_tport_r);

	ast_log(LOG_WARNING, "RADIOHYT test transports are closed\n");

	pipe_close(&test_pipe_w);
	pipe_close(&test_pipe_r);

end:
	if (rc) {
		ast_test_status_update(test, "something wrong");
		rc = AST_TEST_FAIL;
	} else {
		rc = AST_TEST_PASS;
	}
	return rc;
}

#endif

static void *monitor_proc(void *arg)
{
	Monitor *this = (Monitor *)arg;

	ENV_THREAD_INIT_CHECKER(monitor_thread);
	ENV_THREAD_CHECK(monitor_thread);

	this->sched_ctx = ast_sched_context_create();

	this->thread = pthread_self();

	ENV_ASSERT(this->ioc == NULL);
	this->ioc = io_context_create();
	ENV_ASSERT(this->ioc);

	ast_log(LOG_NOTICE, "RADIOHYT: channel monitor thread 0x%x is starting\n", pthread_self());

	ENV_ASSERT(this->sleep_pipe >= 0);
	this->read_io_id = ast_io_add(this->ioc, this->sleep_pipe, io_cmd_callback, AST_IO_IN, this);

	/* monitor main loop */
	while (this->running) {
		int rc;
		/*ENV_DVAR(int num_events);*/

		/* wait for network io */
		/*ENV_DVAR(num_events = )*/ast_io_wait(this->ioc, RADIOHYT_IO_TIMEOUT_MS);

		/* Wait for sched */
		rc = ast_sched_wait(this->sched_ctx);
		// FIXME: 1000 is correct?
		if (rc >= 0 && rc < 1000) {
			/*ENV_DVAR(num_events = )*/ast_sched_runq(this->sched_ctx);
		}
	}

	if (this->ioc) {
		io_context_destroy(this->ioc);
		this->ioc = NULL;
	}

	ast_sched_context_destroy(this->sched_ctx);
	this->sched_ctx = NULL;

	this->stopped = 1;
	ast_log(LOG_NOTICE, "RADIOHYT: channel monitor thread is stopped\n");

	ENV_THREAD_STOP_CHECKER(monitor_thread);
	ENV_THREAD_RESET_CHECKER(monitor_thread);

	return 0;
}

static int io_cmd_callback(int *id, int fd, short events, void *cbdata)
{
	Monitor *this = (Monitor *)cbdata;
	int rc = 1;
	char c;
	unsigned len = 1;
	Async_command *cmd;

	ENV_ASSERT(id == this->read_io_id);
	ENV_ASSERT(fd == this->sleep_pipe);

	pipe_read(this->sleep_pipe, &c, &len);
	if (!this->running) {
		rc = 0;		/* NB: instruct ast_io to clear callback after returning */
		return rc;
	}
	ast_mutex_lock(&this->mutex);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&this->cmd_list, cmd, entry) {
		AST_LIST_REMOVE_CURRENT(entry);
		ast_mutex_unlock(&this->mutex);
		cmd->rc = handle_command(this, cmd);
		ast_sem_post(&cmd->sem);
		ast_mutex_lock(&this->mutex);
	}
	AST_LIST_TRAVERSE_SAFE_END;
	ast_mutex_unlock(&this->mutex);
	return rc;
}

static int io_tport_callback(int *id, int fd, short events, void *cbdata)
{
	int rc = TPORT_RC_CONTINUE;
	Monitor *this = (Monitor *)cbdata;
	Transport *tport;

	AST_LIST_TRAVERSE(&this->tport_list, tport, entry) {
		if (id == tport->io_id) {
			//ast_log(LOG_DEBUG, "RADIOHYT: monitor: fd: %d, mask: %d, events: %d\n", tport->fd, tport->io_mask, events);
			if (events & AST_IO_OUT) {
				if (tport->th_ready_write) {
					rc = tport->th_ready_write(tport);
					if (rc == TPORT_RC_BREAK)
						break;
				}
			}
			if (events & AST_IO_IN) {
				if (tport->th_ready_read) {
					rc = tport->th_ready_read(tport);
					if (rc == TPORT_RC_BREAK)
						break;
				}
			}
			if (events & AST_IO_HUP) {
				if (tport->th_fin_received)
					tport->th_fin_received(tport);
				break;
			}
			if (events & AST_IO_ERR) {
				int err = 0;
				socklen_t len = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
				if (err && tport->th_error) {
					tport->th_error(tport, err);
				}
			}
			break;
		}
	}
	return rc;
}

static int handle_command(Monitor *this, Async_command *cmd)
{
	int rc = E_OK;
	Transport *tport = cmd->tport;

	ENV_THREAD_CHECK(monitor_thread);

	switch (cmd->command) {
	case CMD_USER_PROC:
	{
		rc = cmd->user_func(cmd->user_ctx);
		break;
	}

	case CMD_ADD:
	{
		Transport *cur;

		ENV_ASSERT(tport->fd >= 0);
		AST_LIST_TRAVERSE(&this->tport_list, cur, entry) {
			if (cur == tport) {
				if (!(tport->io_id = ast_io_change(this->ioc, tport->io_id, tport->fd, io_tport_callback, tport->io_mask, this))) {
					ENV_ASSERT(0);
					return E_FAIL;
				}
				ast_log(LOG_DEBUG, "RADIOHYT: monitor: changed: fd: %d, mask: %d\n",
					tport->fd, tport->io_mask);
				return E_OK;
			}
		}

		ENV_ASSERT(tport->io_id == NULL);
		if (!(tport->io_id = ast_io_add(this->ioc, tport->fd, io_tport_callback, tport->io_mask, this))) {
			ENV_ASSERT(0);
			return E_FAIL;
		}
		AST_LIST_INSERT_TAIL(&this->tport_list, tport, entry);

		ast_log(LOG_DEBUG, "RADIOHYT: monitor:   added: fd: %d, mask: %d, io_id: %p(%d)\n",
			tport->fd, tport->io_mask, tport->io_id, *tport->io_id);
		break;
	}

	case CMD_CLOSE:
	{
		Transport *cur;

		AST_LIST_TRAVERSE_SAFE_BEGIN(&this->tport_list, cur, entry) {
			if (cur == tport) {
				AST_LIST_REMOVE_CURRENT(entry);
				break;
			}
		}
		AST_LIST_TRAVERSE_SAFE_END;
		ast_log(LOG_DEBUG, "RADIOHYT: monitor: removed: fd: %d, mask: %d, io_id: %p(%d)\n",
			tport->fd, tport->io_mask, tport->io_id, *tport->io_id);
		ast_io_remove(this->ioc, tport->io_id);
		tport->io_id = NULL;
		break;
	}

	case CMD_ABORT:
	{
		Async_command *c;

		ast_mutex_lock(&this->mutex);
		AST_LIST_TRAVERSE_SAFE_BEGIN(&this->cmd_list, c, entry) {
			if (!cmd->tport || (c->tport == cmd->tport)) {
				AST_LIST_REMOVE_CURRENT(entry);
				rc = E_ABORTED;
				ast_log(LOG_WARNING, "RADIOHYT: monitor: aborted: cmd: %d\n", c->command);
				ast_mutex_unlock(&this->mutex);
				ast_sem_post(&c->sem);
				ast_mutex_lock(&this->mutex);
			}
		}
		AST_LIST_TRAVERSE_SAFE_END;
		ast_mutex_unlock(&this->mutex);
	}

	}
	return rc;
}

static inline void wakeup(Monitor *this)
{
	unsigned len = 1;
	pipe_write(this->wakeup_pipe, "w", &len);
}

static int run_command(Monitor *this, Async_command *cmd)
{
	if (this->thread == pthread_self()) {
		return handle_command(this, cmd);
	}
	cmd->rc = E_ABORTED;
	if (this->running) {
		ast_sem_init(&cmd->sem, 0, 0);

		ast_mutex_lock(&this->mutex);
		AST_LIST_INSERT_TAIL(&this->cmd_list, cmd, entry);
		ast_mutex_unlock(&this->mutex);

		wakeup(this);
		ast_sem_wait(&cmd->sem);

		ast_sem_destroy(&cmd->sem);
	}
	return cmd->rc;
}

int monitor_append_tport(Transport *tport)
{
	Async_command cmd = {
	    .command = CMD_ADD,
	    .tport = tport,
	};
	return run_command(monitor, &cmd);
}

void monitor_remove_tport(Transport *tport)
{
	Async_command cmd = {
	    .command = CMD_CLOSE,
	    .tport = tport,
	};
	run_command(monitor, &cmd);
}

void monitor_abort_tport(Transport *tport)
{
	Async_command cmd = {
	    .command = CMD_ABORT,
	    .tport = tport,
	};
	run_command(monitor, &cmd);
}

int monitor_synch_task(Transport *tport, int (*user_func)(void*), void *user_ctx)
{
	Async_command cmd = {
	    .command = CMD_USER_PROC,
	    .tport = tport,
	    .user_func = user_func,
	    .user_ctx = user_ctx,
	};
	return run_command(monitor, &cmd);
}

struct ast_sched_context *monitor_sched_ctx(void)
{
    return monitor->sched_ctx;
}

#define E_OK (0)
#define E_WAIT (EAGAIN)

/*
 * ����� ������� ��� ������ � POSIX pipe
 */
static int create_pipe(int *reader, int *writer)
{
	int rc;
	int pfd[2];

	if (pipe(pfd) < 0) {
		return errno;
	}

	if (fcntl(pfd[0], F_SETFL, O_NONBLOCK) < 0) {
		rc = errno;
		goto fail;
	}

	if (fcntl(pfd[1], F_SETFL, O_NONBLOCK) < 0) {
		rc = errno;
		goto fail;
	}

	if (reader >= 0) {
		pipe_close(reader);
	}
	if (writer >= 0) {
		pipe_close(writer);
	}
	*reader = pfd[0];
	*writer = pfd[1];
	return E_OK;

fail:
	close(pfd[0]);
	close(pfd[1]);
	return rc;
}

static inline int pipe_read(int fd, void *buf, unsigned *len)
{
	long rc = read(fd, buf, *len);
	if (rc < 0) {
		rc = errno;
		return rc == EAGAIN ? E_WAIT : rc;
	}
	*len = rc;
	return E_OK;
}

static inline int pipe_write(int fd, const void *buf, unsigned *len)
{
	ssize_t rc = write(fd, buf, *len);
	if (rc < 0) {
		rc = errno;
		return rc == EAGAIN ? E_WAIT : rc;
	}
	*len = rc;
	return E_OK;
}

static inline void pipe_close(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static inline void pipe_init(int *fd)
{
	*fd = -1;
}


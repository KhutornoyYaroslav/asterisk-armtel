#include <stdlib.h>

#include <stdio.h>
#include <memory.h>
#include <netinet/in.h>

#include "asterisk.h"
#include "asterisk/logger.h"
#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/rtp_engine.h"
#include "asterisk/netsock2.h"
#include "asterisk/callerid.h"
#include "asterisk/cli.h"
#include "asterisk/causes.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"
#include "asterisk/stringfields.h"
#include "asterisk/abstract_jb.h"
#include "asterisk/threadstorage.h"
#include "asterisk/devicestate.h"
#include "asterisk/indications.h"
#include "asterisk/linkedlists.h"
#include "asterisk/bridge.h"
#include "asterisk/parking.h"
#include "asterisk/unaligned.h"
#ifdef AST_NATIVE_VERSION
#include "asterisk/format_cache.h"
#endif

#include "env_debug.h"
#include "env_error.h"
#include "chan_radiohyt_functions.h"

#define AST_MODULE "chan_radiohyt"

#include "../../include/asterisk/rtp_engine.h"
#include "../../include/asterisk/unaligned.h"
#include "../../include/asterisk/linkedlists.h"
#include "../../include/asterisk/logger.h"
#include "../../include/asterisk/json.h"
#include "../../include/asterisk/channel.h"
#include "../../include/asterisk/netsock2.h"
#include "../../include/asterisk/causes.h"
#include "../../include/asterisk/utils.h"
#include "../../include/asterisk/options.h"

#define USE_RTP_ENGINE // todo: move into makefile

#define SET_TIMER(NAME, SCHED_ID, TIMEOUT, CALLBACK) \
	if (this->TIMEOUT) {\
		ENV_ASSERT(this->SCHED_ID == -1);\
		this->SCHED_ID = ast_sched_add(monitor_sched_ctx(), this->TIMEOUT * 1000, CALLBACK, this);\
		ast_debug(1, "RADIOHYT[%s]: set %s timer(%d) to %d seconds\n", this->name, NAME, this->SCHED_ID, this->TIMEOUT);\
	}

#define RESET_TIMER(SCHED_ID) \
	if (this->SCHED_ID != -1) {\
		ast_debug(1, "RADIOHYT[%s]: reset timer(%d)\n", this->name, this->SCHED_ID);\
		AST_SCHED_DEL(monitor_sched_ctx(), this->SCHED_ID);\
	}

struct chan_make_call_function_arg {
	Channel *chan;
	int chan_id;
	int slot_id;
	int dest_id;
	int call_type;
};

struct chan_get_function_arg {
	Channel *chan;
	int option;
};

#ifdef TEST_FRAMEWORK
struct chan_fake_recv_function_arg {
	Channel *chan;
	struct ast_json *msg;
};

struct chan_session_get_function_arg {
	Channel *chan;
	int channel_id;
	RadiohytSession *session;
};
#endif

struct chan_check_radio_function_arg {
	Channel *chan;
	int chan_id;
	int radio_id;
};

static int  radiohyt_chan_start(void *arg);
static int  radiohyt_chan_stop(void *arg);
static int  radiohyt_chan_make_call(void *arg);
static int  radiohyt_chan_drop_call(void *arg);
static int  radiohyt_chan_load_subscribers(void *arg);
static int  radiohyt_chan_load_groups(void *arg);
static int  radiohyt_chan_load_stations(void *arg);
static int  radiohyt_chan_check_radio(void *arg);
static int  radiohyt_chan_request_timeout(void *arg);
#ifdef TEST_FRAMEWORK
static int  radiohyt_chan_fake_on_recv(void *arg);
static int  radiohyt_chan_fake_on_break(void *arg);
static int  radiohyt_chan_fake_on_connect(void *arg);
static int  radiohyt_chan_session_get(void *arg);
#endif
static void radiohyt_chan_ev_on_recv(Channel *this, struct ast_json *msg);
static void radiohyt_chan_ev_on_break(Channel *this);
static void radiohyt_chan_ev_timeout(Channel *this);
static void radiohyt_chan_ev_stop(Channel *this);

#ifndef TEST_FRAMEWORK
static int  radiohyt_chan_send_message(Channel *this, const char *data, int size);
#else
static int  radiohyt_chan_fake_send_message(Channel *this, struct ast_json *msg);
#endif
static int  radiohyt_chan_send_request(Channel *this, int method, struct ast_json *msg_params, int magic);
static int  radiohyt_chan_send_response(Channel *this, int id, struct ast_json *msg_result);

static int  radiohyt_chan_state_timeout_cb(const void *data);
static int  radiohyt_chan_state_timeout(void *arg);
static void radiohyt_chan_switch_to_state(Channel *this, channel_state newstate);
static int  radiohyt_chan_fsm(Channel *this, channel_event ev, void *arg);

static int  radiohyt_tport_connect(void *arg);
static int  radiohyt_tport_disconnect(void *arg);

static int  radiohyt_tport_ready_read(Transport *tport);
static int  radiohyt_tport_ready_write(Transport *tport);
static void radiohyt_tport_fin_received(Transport *tport);
static void radiohyt_tport_error(Transport *tport, int rc);
static void radiohyt_tport_close(Transport *tport);

#ifndef USE_RTP_ENGINE
static int  radiohyt_rtp_tport_ready_read(Transport *tport);
static int  radiohyt_rtp_tport_ready_write(Transport *tport, struct ast_frame *f);
static void radiohyt_rtp_tport_fin_received(Transport *tport);
static void radiohyt_rtp_tport_error(Transport *tport, int rc);
#endif

static void clean_state_timers(Channel *this);
static void clean_active_requests(Channel *this);
static void clean_active_sessions(Channel *this);

static void remove_session(Channel *this, RadiohytSession *session);

static RadiohytSession *find_session_by_request_id(int request_id);
static RadiohytSession *find_session_by_channel_id(int channel_id);

static void handle_channel_status(Channel *this, struct ast_json *params);
static void handle_subscriber_status(Channel *this, struct ast_json *params);
static void handle_call_state(Channel *this, struct ast_json *params);
static void handle_call_session_established(Channel *this, struct ast_json *params);

// List of active sessions
AST_LIST_HEAD_NOLOCK(, radiohyt_session) radiohyt_session_queue;

/*
 * Interface functions
 */

void radiohyt_session_storage_start(void)
{
	AST_LIST_HEAD_INIT_NOLOCK(&radiohyt_session_queue);
}

void radiohyt_session_storage_stop(void)
{
	RadiohytSession *item;

	while ((item = AST_LIST_REMOVE_HEAD(&radiohyt_session_queue, next))) {
		ast_free(item);
	}
}

int radiohyt_chan_open(Channel *chan)
{
	ast_log(LOG_NOTICE, "RADIOHYT[%s]: open channel\n", chan->name);
	return monitor_synch_task(&chan->tport, radiohyt_chan_start, chan);
}

void radiohyt_chan_close(Channel *chan)
{
	ast_log(LOG_NOTICE, "RADIOHYT[%s]: close channel\n", chan->name);
	monitor_synch_task(&chan->tport, radiohyt_chan_stop, chan);
}

int radiohyt_make_call(Channel *chan, int chan_id, int slot_id, int dest_id, int call_type, int timeout_ms)
{
	struct chan_make_call_function_arg arg = {
		.chan = chan,
		.chan_id = chan_id,
		.slot_id = slot_id,
		.dest_id = dest_id,
		.call_type = call_type,
	};

	int rc = monitor_synch_task(&chan->tport, radiohyt_chan_make_call, &arg);
#ifdef TEST_FRAMEWORK
	usleep(timeout_ms*1000);
#endif
	return rc;
}

int radiohyt_drop_call(Channel *chan, int chan_id, int slot_id, int dest_id, int call_type, int timeout_ms)
{
	struct chan_make_call_function_arg arg = {
		.chan = chan,
		.chan_id = chan_id,
		.slot_id = slot_id,
		.dest_id = dest_id,
		.call_type = call_type,
	};

	int rc = monitor_synch_task(&chan->tport, radiohyt_chan_drop_call, &arg);
#ifdef TEST_FRAMEWORK
	usleep(timeout_ms*1000);
#endif
	return rc;
}

int radiohyt_get_subscribers(Channel *chan, int option)
{
	struct chan_get_function_arg arg = {
		.chan = chan,
		.option = option,
	};

	return monitor_synch_task(&chan->tport, radiohyt_chan_load_subscribers, &arg);
}

int radiohyt_get_groups(Channel *chan, int option)
{
	struct chan_get_function_arg arg = {
		.chan = chan,
		.option = option,
	};

	return monitor_synch_task(&chan->tport, radiohyt_chan_load_groups, &arg);
}

int radiohyt_get_stations(Channel *chan, int option)
{
	struct chan_get_function_arg arg = {
		.chan = chan,
		.option = option,
	};

	return monitor_synch_task(&chan->tport, radiohyt_chan_load_stations, &arg);
}

int radiohyt_check_radio(Channel *chan, int chan_id, int radio_id, int timeout_ms)
{
	struct chan_check_radio_function_arg arg = {
		.chan = chan,
		.chan_id = chan_id,
		.radio_id = radio_id,
	};

	int rc = monitor_synch_task(&chan->tport, radiohyt_chan_check_radio, &arg);
#ifdef TEST_FRAMEWORK
	usleep(timeout_ms*1000);
#endif
	return rc;
}

#ifdef TEST_FRAMEWORK
void radiohyt_fake_recv_event(Channel *chan, struct ast_json *msg, int timeout_ms)
{
	struct chan_fake_recv_function_arg arg = {
		.chan = chan,
		.msg = msg,
	};

	monitor_synch_task(&chan->tport, radiohyt_chan_fake_on_recv, &arg);
	usleep(timeout_ms*1000);
}

void radiohyt_fake_on_break_event(Channel *chan)
{
	struct chan_fake_recv_function_arg arg = {
		.chan = chan,
	};

	monitor_synch_task(&chan->tport, radiohyt_chan_fake_on_break, &arg);
}

void radiohyt_fake_on_connect_event(Channel *chan)
{
	struct chan_fake_recv_function_arg arg = {
		.chan = chan,
	};

	monitor_synch_task(&chan->tport, radiohyt_chan_fake_on_connect, &arg);
}

int radiohyt_session_get(Channel *chan, int channel_id, RadiohytSession *session)
{
	struct chan_session_get_function_arg arg = {
		.chan = chan,
		.channel_id = channel_id,
		.session = session,
	};
	return monitor_synch_task(&chan->tport, radiohyt_chan_session_get, &arg);
}
#endif

/*
 * internal functions
 */
enum {
    RET_VAL_RESHEDULE_OFF = 0,
    RET_VAL_RESHEDULE_ON = 1,
};

static int radiohyt_connect_timeout_cb(const void *data)
{
	Channel *this = (Channel *)data;

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: connection timeout(%d)\n",
		this->name, this->sched_conn_id);

	monitor_synch_task(0, radiohyt_tport_disconnect, this);
	usleep(1000);
	if (monitor_synch_task(0, radiohyt_tport_connect, this)) {
		return RET_VAL_RESHEDULE_ON;
	}

	this->sched_conn_id = -1;

	return RET_VAL_RESHEDULE_OFF;
}

static int radiohyt_request_timeout_cb(const void *data)
{
	RadiohytRequest *request = (RadiohytRequest *)data;
	Channel *this = (Channel *)request->this;
	RadiohytRequest *item;

	ast_log(LOG_ERROR, "RADIOHYT[%s]: request(%d) timeout\n",
		this->name, request->sched_id);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&this->request_queue, item, next) {
		if (item == request) {
			AST_LIST_REMOVE_CURRENT(next);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;
	//todo: free data

	if (monitor_synch_task(0, radiohyt_chan_request_timeout, this)) {
		return RET_VAL_RESHEDULE_OFF;
	}
#ifndef TEST_FRAMEWORK
	monitor_synch_task(0, radiohyt_tport_disconnect, this);
	usleep(1000);
	if (monitor_synch_task(0, radiohyt_tport_connect, this)) {
		return RET_VAL_RESHEDULE_OFF;
	}
#endif
	return RET_VAL_RESHEDULE_OFF;
}

static void clean_state_timers(Channel *this)
{
	RadiohytChannelState *pstate;
	int i;

	for (i = 0; i < sizeof(this->states)/sizeof(this->states[0]); ++i) {
		pstate = &this->states[i];

		// reset current state's timer
		if (pstate->timer_id != -1) {
			ast_debug(2, "RADIOHYT[%s]: reset state(%s) timer(%d)\n",
				this->name, channel_state_to_str(pstate->state), pstate->timer_id);
			AST_SCHED_DEL(monitor_sched_ctx(), pstate->timer_id);
		}
	}
}

static void clean_active_requests(Channel *this)
{
	RadiohytRequest *item;

	while ((item = AST_LIST_REMOVE_HEAD(&this->request_queue, next)) != NULL) {
		if (this->debug)
			ast_log(LOG_VERBOSE, "RADIOHYT[%s]: reset request(%s) timer(%d)\n",
				this->name, hyterus_msg_type_to_str(item->method), item->sched_id);
		AST_SCHED_DEL(monitor_sched_ctx(), item->sched_id);
		ast_free(item);
	}
}

static void clean_active_sessions(Channel *this)
{
	RadiohytSession *item;

	this->session = NULL;
	this->sid[0] = '\0';
	this->peer_radio_id = 0;
	this->peer_channel_id = 0;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&radiohyt_session_queue, item, next) {
		if (item->owner == this ||
		    item->status_owner == this ||
		    (item->owner == NULL && item->status_owner == NULL)) {
			if (this->on_changed_call_state_cb)
				this->on_changed_call_state_cb(this, item->source, item->receiver_radio_id, CallInterrupted);

			ast_debug(1, "RADIOHYT[%s]: remove call on channel_id %d due stop or break connection\n",
				this->name, item->channel_id);

			AST_LIST_REMOVE_CURRENT(next);
			ast_free(item);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

static void remove_session(Channel *this, RadiohytSession *session)
{
	RadiohytSession *item;

	if (this->session == session)
		this->session = NULL;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&radiohyt_session_queue, item, next) {
		if (session == item) {
			ast_assert(item->status_owner == NULL || item->status_owner == this);
			AST_LIST_REMOVE_CURRENT(next);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	ast_free(session);
}

static RadiohytSession *find_session_by_request_id(int request_id)
{
	RadiohytSession *item;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&radiohyt_session_queue, item, next) {
		if (request_id && item->request_id == request_id)
			return item;
	}
	AST_LIST_TRAVERSE_SAFE_END;

	return NULL;
}

static RadiohytSession *find_session_by_channel_id(int channel_id)
{
	RadiohytSession *item;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&radiohyt_session_queue, item, next) {
		if (channel_id && item->channel_id == channel_id)
			return item;
	}
	AST_LIST_TRAVERSE_SAFE_END;

	return NULL;
}

/*
 * Send message function
 */
static inline int next_transaction_id(Channel *this)
{
	if (this->last_transaction_id == INT_MAX)
		this->last_transaction_id = 0;
	return ++this->last_transaction_id;
}

static int radiohyt_chan_send_request(Channel *this, int method, struct ast_json *msg_params, int magic)
{
	int rc;
	int size;
	RAII_VAR(char *, data, NULL, ast_json_free);
	RAII_VAR(struct ast_json *, msg, NULL, ast_json_unref);

	msg = ast_json_object_create();
	ast_json_object_set(msg, "jsonrpc", ast_json_string_create("2.0"));
	ast_json_object_set(msg, "method", ast_json_string_create(hyterus_msg_type_to_str(method)));
	if (msg_params)
		ast_json_object_set(msg, "params", msg_params);
	ast_json_object_set(msg, "id", ast_json_integer_create(next_transaction_id(this)));

	data = ast_json_dump_string_format(msg, AST_JSON_PRETTY);
	size = strlen(data);
	ast_assert(size);
#ifndef TEST_FRAMEWORK
	if (this->debug)
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: tcp: sending to: %s\n%s\n\n",
			this->name, ast_sockaddr_stringify(&this->srv_addr), data);
	rc = radiohyt_chan_send_message(this, data, size);
#else
	if (this->debug)
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: fake: sending:\n%s\n\n",
			this->name, data);
	//todo: do we need to call ast_json_ref(msg_params)?!
	rc = radiohyt_chan_fake_send_message(this, ast_json_ref(msg));
#endif
	if (rc == E_OK) {
		RadiohytRequest *request = (RadiohytRequest *)ast_calloc(1, sizeof(RadiohytRequest));

		request->this = this;
		request->method = method;
		request->transaction_id = this->last_transaction_id;
		request->sched_id = ast_sched_add(monitor_sched_ctx(), this->request_timeout * 1000, radiohyt_request_timeout_cb, request);
		request->magic = magic;

		if (this->debug)
			ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set request(%s) timer(%d)\n",
				this->name, hyterus_msg_type_to_str(method), request->sched_id);

		AST_LIST_INSERT_TAIL(&this->request_queue, request, next);
	}
	return rc;
}

int radiohyt_chan_send_response(Channel *this, int id, struct ast_json *msg_result)
{
	int rc;
	int size;
	RAII_VAR(char *, data, NULL, ast_json_free);
	RAII_VAR(struct ast_json *, msg, NULL, ast_json_unref);

	msg = ast_json_object_create();
	ast_json_object_set(msg, "jsonrpc", ast_json_string_create("2.0"));
	if (msg_result)
		ast_json_object_set(msg, "result", msg_result);
	if (id) {
		if (this->debug)
			ast_log(LOG_VERBOSE, "RADIOHYT[%s]: response id: %d\n",
				this->name, id);
		ast_json_object_set(msg, "id", ast_json_integer_create(id));
	}

	data = ast_json_dump_string_format(msg, AST_JSON_PRETTY);
	size = strlen(data);
	ast_assert(size);
#ifndef TEST_FRAMEWORK
	if (this->debug)
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: tcp: sending to: %s\n%s\n\n",
			this->name, ast_sockaddr_stringify(&this->srv_addr), data);
	rc = radiohyt_chan_send_message(this, data, size);
#else
	if (this->debug)
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: fake: sending:\n%s\n\n",
			this->name, data);
	//todo: do we need to call ast_json_ref(msg_result)?!
	rc = radiohyt_chan_fake_send_message(this, ast_json_ref(msg));
#endif
    return rc;
}

#ifndef TEST_FRAMEWORK
int radiohyt_chan_send_message(Channel *this, const char *data, int size)
{
	int rc = E_OK;
	int ix = 0;
	int timeout = TCP_SEND_TIMEOUT_USEC;

	while (size > 0) {
		int rc = send(this->tport.fd, &data[ix], size, 0);
		if (rc > 0) {
			size -= rc;
			ix += rc;
			rc = E_OK;
		} else if (rc == EAGAIN) {
			if (timeout > 0) {
				struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
				timeout -= tv.tv_usec;
				select(0, 0, 0, 0, &tv);
			} else {
				rc = E_TIMEOUT;
				break;
			}
		} else {
			rc = E_FAIL;
			break;
		}
	}
	if (!rc) {
		gettimeofday(&this->tport.stat.last_send_ts, 0);
		++this->tport.stat.send_msg_cnt;
	}
	return rc;
}
#else
int radiohyt_chan_fake_send_message(Channel *this, struct ast_json *msg)
{
	ast_assert(this->last_send_msg == NULL);
	this->last_send_msg = msg;
	return E_OK;
}
#endif // TEST_FRAMEWORK

/*
 * RADIOHYT Channel FSM
 */
int radiohyt_chan_state_timeout_cb(const void *data)
{
	Channel *this = (Channel *)data;

	ast_debug(2, "RADIOHYT: channel state timeout expired\n");

	return monitor_synch_task(0, radiohyt_chan_state_timeout, this);
}

int radiohyt_chan_state_timeout(void *arg)
{
	Channel *this = (Channel *)arg;

	ENV_THREAD_CHECK(monitor_thread);

	ast_log(LOG_WARNING, "RADIOHYT[%s]: channel state '%s' timeout(%d) expired\n",
		this->name, channel_state_to_str(this->states[this->state].state),
		this->states[this->state].timer_id);

	this->states[this->state].timer_id = -1;

	radiohyt_chan_fsm(this, EV_ON_CHAN_STATE_TIMEOUT, NULL);

	return RET_VAL_RESHEDULE_OFF;
}

void radiohyt_chan_switch_to_state(Channel *this, channel_state newstate)
{
	RadiohytChannelState *pstate;

	ENV_THREAD_CHECK(monitor_thread);

	if (this->state == newstate)
		return;

	pstate = &this->states[this->state];

	// reset current state's timer
	if (pstate->timer_id != -1) {
		ast_debug(2, "RADIOHYT[%s]: reset state(%s) timer(%d)\n",
			this->name, channel_state_to_str(pstate->state), pstate->timer_id);
		AST_SCHED_DEL(monitor_sched_ctx(), pstate->timer_id);
	}

	// set new state
	this->state = newstate;
	pstate = &this->states[this->state];

	gettimeofday(&pstate->timestamp, 0);
	ast_log(LOG_NOTICE, "RADIOHYT[%s]: switched to state: %s\n",
		this->name, channel_state_to_str(this->state));

	// set new state's timer
	if (pstate->timeout) {
		ENV_ASSERT(pstate->timer_id == -1);
		pstate->timer_id = ast_sched_add(monitor_sched_ctx(),
			pstate->timeout * 1000, radiohyt_chan_state_timeout_cb, this);
		ast_debug(2, "RADIOHYT[%s]: set state(%s) timer(%d) to %d seconds\n",
			this->name, channel_state_to_str(pstate->state), pstate->timer_id, pstate->timeout);
	}
}

int radiohyt_chan_fsm(Channel *this, channel_event ev, void *arg)
{
	int rc = E_OK;

	ENV_THREAD_CHECK(monitor_thread);

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: recv event: %s into state: %s\n",
		this->name, channel_event_to_str(ev), channel_state_to_str(this->state));

	switch (this->state) {
	case CHAN_RADIOHYT_ST_IDLE:
		if (ev == EV_START) {
			this->tport.fd = -1;
			this->tport.io_id = NULL;
			this->tport.owner = this;

			this->rtp_tport.fd = -1;
			this->rtp_tport.io_id = NULL;
			this->rtp_tport.owner = this;

			this->sched_conn_id = -1;

			gettimeofday(&this->tport.stat.disconnection_ts, 0);
#ifndef TEST_FRAMEWORK
			rc = radiohyt_tport_connect(this);

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_TRYC);

			if (rc) {
				SET_TIMER("connect", sched_conn_id, connect_timeout, radiohyt_connect_timeout_cb);
			}
#else
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
#endif
		} else {
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_TRYC:
		switch (ev) {
		case EV_ON_CONNECT:
		{
			this->last_transaction_id = 0;

			this->sched_conn_id = -1;

			++this->tport.stat.connections_cnt;
			gettimeofday(&this->tport.stat.connection_ts, 0);

			init_parser(&this->parser, HYTERUS_MSG_MAX_SIZE);
#ifndef TEST_FRAMEWORK
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_CONN);
#else
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
#endif
			break;
		}
		case EV_ON_CONNECT_FAIL:
			SET_TIMER("connect", sched_conn_id, connect_timeout, radiohyt_connect_timeout_cb);
			break;
		case EV_ON_CHAN_STATE_TIMEOUT:
			break;
		case EV_STOP:
			radiohyt_chan_ev_stop(this);
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_CONN:
		switch (ev) {
		case EV_ON_AUTHENTICATION_REQUEST:
		{
#define SALT_LEN 16
#define HASH_LEN 32
#define RESP_LEN (((SALT_LEN) * 2) + (HASH_LEN))
			int id = -1;
			char salt_s[SALT_LEN+1], salt_c[SALT_LEN+1], md5_hash[HASH_LEN+1], hash[RESP_LEN+1];
			struct ast_json *msg_params = ast_json_object_create();
			struct ast_json *recv_msg = (struct ast_json *)arg;
			ENV_ASSERT(recv_msg);

			memset(salt_c, 0, sizeof(salt_c));
			memset(hash,   0, sizeof(hash));
			memset(salt_s, 0, sizeof(salt_s));

			if (recv_msg) {
				struct ast_json *params = ast_json_object_get(recv_msg, "params");
				if (params) {
					const struct ast_json *recv_salt = ast_json_object_get(params, "Salt");
					if (recv_salt) {
						const char *salt = ast_json_string_get(recv_salt);
						if (salt) {
							strncpy(salt_s, salt, sizeof(salt_s));
						}
					}
				} else {
					ENV_ASSERT(0);
				}
				id = ast_json_integer_get(ast_json_object_get(recv_msg, "id"));
			}

			snprintf(salt_c, sizeof(salt_c), "%08lx%08lx", (unsigned long)ast_random(), (unsigned long)ast_random());
			snprintf(hash, sizeof(hash), "%s%s%s", salt_s, salt_c, this->md5secret);
			ast_md5_hash(md5_hash, hash);

			ast_json_object_set(msg_params, "Login", ast_json_string_create(this->login));
			ast_json_object_set(msg_params, "Salt", ast_json_string_create(salt_c));
			ast_json_object_set(msg_params, "Hash", ast_json_string_create(md5_hash));
			ast_json_object_set(msg_params, "ClientUdpPort", ast_json_integer_create(this->rtp_port));

			radiohyt_chan_send_response(this, id, msg_params);

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_WFA);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_BREAK:
		case EV_STOP:
			radiohyt_chan_ev_stop(this);
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_WFA:
		switch (ev) {
		case EV_ON_AUTHENTICATION_RESPONSE:
		{
			int auth_res_code = -1, proto_version = 0;
			struct ast_sockaddr srv_addr;
			struct ast_json *params;
			struct ast_json *recv_msg = (struct ast_json *)arg;
			ENV_ASSERT(recv_msg);

			params = ast_json_object_get(recv_msg, "params");
			ENV_ASSERT(params);

			auth_res_code = ast_json_integer_get(ast_json_object_get(params, "AuthenticationResult"));
			if (auth_res_code) {
				ast_log(LOG_WARNING, "RADIOHYT[%s]: authentication failed with code: %d(%s)\n",
					this->name, auth_res_code, hyterus_auth_result_type_to_str(auth_res_code));
				break;
			}
			this->srv_rtp_port = ast_json_integer_get(ast_json_object_get(params, "ServerUdpPort"));
			proto_version = ast_json_integer_get(ast_json_object_get(params, "ProtocolVersion"));

			ast_log(LOG_NOTICE, "RADIOHYT[%s]: authentication is successfully completed, protocol version is %x\n",
				this->name, proto_version);

			ast_sockaddr_copy(&srv_addr, &this->srv_addr);
			ast_sockaddr_set_port(&srv_addr, this->srv_rtp_port);

#ifdef USE_RTP_ENGINE
			if (this->rtpi) {
				ast_log(LOG_NOTICE, "RADIOHYT: rtp instance: %p connecting to: %s\n",
					this->rtpi, ast_sockaddr_stringify(&srv_addr));
# ifdef AST_NATIVE_VERSION
				if (ast_rtp_instance_set_incoming_source_address(this->rtpi, &srv_addr))
					ast_log(LOG_ERROR, "RADIOHYT: rtp: can't use remote address: %s\n",
						ast_sockaddr_stringify(&srv_addr));
				
				if (ast_rtp_instance_set_requested_target_address(this->rtpi, &srv_addr))
					ast_log(LOG_ERROR, "RADIOHYT: rtp: can't use remote address: %s\n",
						ast_sockaddr_stringify(&srv_addr));
# else
				if (ast_rtp_instance_set_remote_address(this->rtpi, &srv_addr))
					ast_log(LOG_ERROR, "RADIOHYT: rtp: can't use remote address: %s\n",
						ast_sockaddr_stringify(&srv_addr));
# endif
			}
#else
			if (ast_connect(this->rtp_tport.fd, &srv_addr) < 0) {
				ast_log(LOG_ERROR, "RADIOHYT: rtp: can't connect to %s: %s\n",
					ast_sockaddr_stringify(&srv_addr),
					strerror(errno));
				ENV_ASSERT(0);
			}

			if (fcntl(this->rtp_tport.fd, F_SETFL, O_NONBLOCK)) {
				ast_log(LOG_ERROR, "RADIOHYT: rtp: can't set O_NONBLOCK option: %s\n",
					strerror(errno));
				ENV_ASSERT(0);
			}

			if (this->rtp_tport.io_id == NULL) {
				this->rtp_tport.io_mask = /*AST_IO_OUT | */AST_IO_IN;

				this->rtp_tport.th_ready_write  = NULL;//radiohyt_rtp_tport_ready_write;
				this->rtp_tport.th_ready_read   = radiohyt_rtp_tport_ready_read;
				this->rtp_tport.th_fin_received = radiohyt_rtp_tport_fin_received;
				this->rtp_tport.th_error        = radiohyt_rtp_tport_error;

				if (monitor_append_tport(&this->rtp_tport)) {
					ast_log(LOG_ERROR, "RADIOHYT: can't append transport into monitor\n");
					radiohyt_tport_close(&this->rtp_tport);
					ENV_ASSERT(0);
				}
			}
#endif
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);

			if (this->on_changed_connection_state_cb)
				this->on_changed_connection_state_cb(this, this->state);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_BREAK:
		case EV_STOP:
			radiohyt_chan_ev_stop(this);
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_READY:
		switch (ev) {
		case EV_MAKE_CALL:
		{
			struct chan_make_call_function_arg *farg = (struct chan_make_call_function_arg *)arg;
			struct ast_json *msg_params = ast_json_object_create();

			ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(farg->chan_id));
			ast_json_object_set(msg_params, "CallType", ast_json_integer_create(farg->call_type));
			ast_json_object_set(msg_params, "ReceiverRadioId", ast_json_integer_create(farg->dest_id));

			radiohyt_chan_send_request(this, HYTERUS_REQUEST_START_VOICE_CALL, msg_params, 0);

			// store session information
			{
				RadiohytSession *session = (RadiohytSession *)ast_calloc(1, sizeof(*session));
				session->owner = this;
				session->status_owner = this;
				session->caller_id[0] = '\0';
				if (this->peer) {
					struct ast_party_caller *caller = ast_channel_caller(this->peer);
					strcpy(session->caller_id, caller->id.number.str);
				}
				strcpy(session->source, this->login);
				session->receiver_radio_id = farg->dest_id;
				session->channel_id = farg->chan_id;
				session->request_id = this->last_transaction_id;
				session->direction = outgoing;
				AST_LIST_INSERT_TAIL(&radiohyt_session_queue, session, next);

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: create[1] outgoing voice session on channel %d to radio_id %d\n",
					this->name, session->channel_id, session->receiver_radio_id);

				this->session = session;
			}

			this->direction = outgoing;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_DIAL);
			break;
		}
		case EV_ON_CALL_SESSION_EST:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);


			if (this->on_incoming_call_cb) {
				ast_log(LOG_NOTICE, "RADIOHYT[%s]: incoming call on channel %d from %s to %d is processing\n",
					this->name, session->channel_id, session->source, session->receiver_radio_id);

				if (this->on_incoming_call_cb(this, &this->srv_addr,
                                              session->source, session->receiver_radio_id,
                                              session->channel_id, session->caller_id) == E_OK) {

					this->session = session;

					session->owner = this;
					session->status_owner = this;

					ast_assert(this->direction == incoming);
					ast_assert(this->peer_channel_id == session->channel_id);

					++this->tport.stat.calls_cnt;
					gettimeofday(&this->tport.stat.voice_request_answer_ts, 0);

					ast_log(LOG_NOTICE, "RADIOHYT[%s]: create[2] incoming voice session on channel %d from radio_id %s\n",
						this->name, session->channel_id, session->source);

					radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_BUSY);
				} else {
					session->owner = NULL;
					session->status_owner = NULL;
				}
			}
			break;
		}
		case EV_CHECK_RADIO:
		{
			struct chan_check_radio_function_arg *farg = (struct chan_check_radio_function_arg *)arg;
			struct ast_json *msg_params = ast_json_object_create();

			//ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(farg->chan_id));
			//ast_json_object_set(msg_params, "RadioId", ast_json_integer_create(farg->radio_id));

			radiohyt_chan_send_request(this, HYTERUS_REQUEST_PING, msg_params, 0);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_REQUEST_TIMEOUT:
			monitor_abort_tport(&this->tport);
			radiohyt_chan_ev_timeout(this);
			break;
		case EV_STOP:
			radiohyt_chan_ev_stop(this);
			break;
		case EV_ON_BREAK:
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_DIAL:
		switch (ev) {
		case EV_ON_MAKE_CALL_RESPONSE:
		{
			struct ast_json *result;
			struct ast_json *recv_msg = (struct ast_json *)arg;
			int request_id;
			ENV_ASSERT(recv_msg);

			request_id = ast_json_integer_get(ast_json_object_get(recv_msg, "id"));
			result = ast_json_object_get(recv_msg, "result");
			rc = ast_json_integer_get(ast_json_object_get(result, "Result"));
			if (rc) {
				RadiohytSession *session = find_session_by_request_id(request_id);
				ast_assert(session != NULL);
				ast_assert(session->owner == this);

				//NB: here we are waiting for response on request we sent early, without any checks!
				ast_log(LOG_WARNING, "RADIOHYT[%s]: outgoing voice session on channel %d to radio_id %d is rejected with code: %d\n",
					this->name, session->channel_id, session->receiver_radio_id, rc);

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[4] voice session on channel %d\n",
					this->name, session->channel_id);

				remove_session(this, session);

				radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
				monitor_abort_tport(&this->tport);

				if (this->owner) {
					switch (rc) {
					case Failure:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_NORMAL_TEMPORARY_FAILURE);
						break;
					case InvalidParameter:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_INVALID_MSG_UNSPECIFIED);
						break;
					case RadioBusy:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_USER_BUSY);
						break;
					case InvalidTargetAddress:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_INCOMPATIBLE_DESTINATION);
						break;
					default:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_PROTOCOL_ERROR);
						ENV_ASSERT(0);
						break;
					}
				}
			} else {
				ast_log(LOG_VERBOSE, "RADIOHYT[%s]: outgoing voice session request(%d) is accepted\n",
					this->name, request_id);

				radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_RING);
			}
			break;
		}
		case EV_ON_CHANNEL_IS_FREED:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			// store new session information
			{
				RadiohytSession *new_session = (RadiohytSession *)ast_calloc(1, sizeof(*new_session));
				new_session->owner = this;
				new_session->status_owner = this;
				strcpy(new_session->caller_id, session->caller_id);
				strcpy(new_session->source, this->login);
				new_session->receiver_radio_id = session->receiver_radio_id;
				new_session->channel_id = session->channel_id;
				new_session->request_id = this->last_transaction_id;
				new_session->direction = outgoing;
				AST_LIST_INSERT_TAIL(&radiohyt_session_queue, new_session, next);

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: create[3] outgoing voice session on channel %d to radio_id %d\n",
					this->name, session->channel_id, session->receiver_radio_id);

				this->session = new_session;
			}
			// StartVoiceCall has been already sent into hangtime, but HandleCallState(3) is received before Response,
			// we still waiting for Response
			break;
		}
		case EV_DROP_CALL:
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_CANCEL);
			break;
		case EV_MAKE_CALL:
			rc = E_BUSY;
			break;
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_REQUEST_TIMEOUT:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			radiohyt_chan_ev_timeout(this);
			break;
		case EV_ON_BREAK:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			break;
		case EV_STOP:
			radiohyt_chan_ev_stop(this);
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_RING:
		switch (ev) {
		case EV_MAKE_CALL:
			rc = E_BUSY;
			break;
		case EV_DROP_CALL:
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_CANCEL);
			break;
		case EV_ON_CALL_SESSION_EST:
		{
			uint64_t msec;
			Transport *tport = &this->tport;

			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			this->peer_radio_id = session->receiver_radio_id;
			this->peer_channel_id = session->channel_id;

			++tport->stat.calls_cnt;

			gettimeofday(&tport->stat.voice_request_answer_ts, 0);
			msec = ast_tvdiff_ms(tport->stat.voice_request_answer_ts, tport->stat.voice_request_ts);
			if (msec < tport->stat.min_answer_time_ms || tport->stat.min_answer_time_ms == 0)
				tport->stat.min_answer_time_ms = msec;
			if (this->tport.stat.max_answer_time_ms < msec)
				tport->stat.max_answer_time_ms = msec;
			tport->stat.sum_answer_time_ms += msec;
			tport->stat.ave_answer_time_ms = tport->stat.sum_answer_time_ms / tport->stat.calls_cnt;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_BUSY);

//			if (this->peer)
//				ast_answer(this->peer);
			if (this->owner)
				ast_queue_control(this->owner, AST_CONTROL_ANSWER);
			break;
		}
		case EV_ON_CALL_SESSION_FAIL:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);
			ast_assert(session->owner == this);

			ast_log(LOG_WARNING, "RADIOHYT[%s]: outgoing voice session on channel %d to radio_id %d is failed\n",
				this->name, session->channel_id, session->receiver_radio_id);

			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_NORMAL_TEMPORARY_FAILURE);
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_REQUEST_TIMEOUT:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			radiohyt_chan_ev_timeout(this);
			break;
		case EV_ON_BREAK:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			break;
		case EV_STOP:
			radiohyt_chan_ev_stop(this);
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_BUSY:
		switch (ev) {
		case EV_DROP_CALL:
		{
			if (this->direction == outgoing) {
				struct ast_json *msg_params = ast_json_object_create();
				ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(this->peer_channel_id));

				radiohyt_chan_send_request(this, HYTERUS_REQUEST_STOP_VOICE_CALL, msg_params, 0);
				radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_DETACH);
			} else {
				//todo: we need to remove ast_chahnel, but store radio channel state
			}
			break;
		}
		case EV_STOP:
		{
			RadiohytSession *session = find_session_by_channel_id(this->peer_channel_id);
			ast_assert(session != NULL);
			ast_assert(session->owner == this);

			if (session && session->owner == this) {
				if (this->direction == outgoing) {
					struct ast_json *msg_params = ast_json_object_create();
					ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(this->peer_channel_id));

					radiohyt_chan_send_request(this, HYTERUS_REQUEST_STOP_VOICE_CALL, msg_params, 0);
				}

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[6] call on channel %d\n",
					this->name, session->channel_id);

				remove_session(this, session);
			}

			radiohyt_chan_ev_stop(this);
			break;
		}
		case EV_ON_CHANNEL_IN_HANGTIME:
		{
			monitor_abort_tport(&this->tport);
			if (this->owner) // todo: maybe, is exists function to set owner, which called from core?
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_NORMAL_CLEARING);
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_HANGTIME);
			break;
		}
		case EV_ON_CHANNEL_IS_FREED:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_NORMAL_CLEARING);

			this->sid[0] = '\0';
			this->peer_radio_id = 0;
			this->peer_channel_id = 0;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
			break;
		}
		case EV_ON_BREAK:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			break;
		case EV_MAKE_CALL:
			rc = E_BUSY;
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_DETACH:
		switch (ev) {
		case EV_MAKE_CALL:
			rc = E_BUSY;
			break;
		case EV_ON_DROP_CALL_RESPONSE:
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_DETACH_2);
			break;
		case EV_ON_CHANNEL_IS_FREED:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			this->sid[0] = '\0';
			this->peer_radio_id = 0;
			this->peer_channel_id = 0;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_REQUEST_TIMEOUT:
			monitor_abort_tport(&this->tport);
			radiohyt_chan_ev_timeout(this);
			break;
		case EV_ON_BREAK:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			break;
		case EV_STOP:
		{
			RadiohytSession *session = find_session_by_channel_id(this->peer_channel_id);
			ast_assert(session != NULL);
			ast_assert(session->owner == this);

			if (session && session->owner == this) {
				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[6] call on channel %d\n",
					this->name, session->channel_id);

				remove_session(this, session);
			}

			radiohyt_chan_ev_stop(this);
			break;
		}
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_DETACH_2:
		switch (ev) {
		case EV_MAKE_CALL:
			rc = E_BUSY;
			break;
		case EV_ON_CHANNEL_IN_HANGTIME:
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_HANGTIME);
			break;
		case EV_ON_CHANNEL_IS_FREED:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			this->sid[0] = '\0';
			this->peer_radio_id = 0;
			this->peer_channel_id = 0;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
			monitor_abort_tport(&this->tport);
			radiohyt_chan_ev_timeout(this);
			break;
		case EV_ON_BREAK:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			break;
		case EV_STOP:
		{
			RadiohytSession *session = find_session_by_channel_id(this->peer_channel_id);
			ast_assert(session != NULL);
			ast_assert(session->owner == this);

			if (session && session->owner == this) {
				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[6] call on channel %d\n",
					this->name, session->channel_id);

				remove_session(this, session);
			}

			radiohyt_chan_ev_stop(this);
			break;
		}
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_HANGTIME:
		switch (ev) {
		case EV_ON_CHANNEL_IS_FREED:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			this->sid[0] = '\0';
			this->peer_radio_id = 0;
			this->peer_channel_id = 0;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
			break;
		}
		case EV_ON_CALL_SESSION_EST:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			if (this->on_incoming_call_cb) {
				ast_log(LOG_NOTICE, "RADIOHYT[%s]: incoming call on channel %d from %s to %d is processing\n",
					this->name, session->channel_id, session->source, session->receiver_radio_id);

				if (this->on_incoming_call_cb(this, &this->srv_addr,
                                              session->source, session->receiver_radio_id,
                                              session->channel_id, session->caller_id) == E_OK) {
					this->session = session;

					ast_assert(session->owner == this);
					ast_assert(session->status_owner == this);

					ast_assert(this->direction == incoming);
					ast_assert(this->peer_channel_id == session->channel_id);

					++this->tport.stat.calls_cnt;
					gettimeofday(&this->tport.stat.voice_request_answer_ts, 0);

					radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_BUSY);
				} else {
					session->owner = NULL;
					session->status_owner = this;

					this->sid[0] = '\0';
					this->peer_radio_id = 0;
					this->peer_channel_id = 0;

					radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
				}
			}
			break;
		}
		case EV_MAKE_CALL:
		{
			struct chan_make_call_function_arg *farg = (struct chan_make_call_function_arg *)arg;
			struct ast_json *msg_params;

			RadiohytSession *session = find_session_by_channel_id(farg->chan_id);
			if (session == NULL) {
				// deny call to another radio subscriber into hangtime, because subscriber can re-call back dispatcher
				rc = E_BUSY;
				break;
			}
			if (session->owner != this) {
				// very strange situation - someone interrupt own channel, this need be investigated if occurs!
				ast_assert(0);
				rc = E_BUSY;
				break;
			}

			msg_params = ast_json_object_create();
			ENV_ASSERT(msg_params);

			ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(farg->chan_id));
			ast_json_object_set(msg_params, "CallType", ast_json_integer_create(farg->call_type));
			ast_json_object_set(msg_params, "ReceiverRadioId", ast_json_integer_create(farg->dest_id));

			radiohyt_chan_send_request(this, HYTERUS_REQUEST_START_VOICE_CALL, msg_params, 0);

			strcpy(session->source, this->login);
			session->receiver_radio_id = farg->dest_id;
			session->request_id = this->last_transaction_id;
			session->direction = outgoing;

			ast_log(LOG_NOTICE, "RADIOHYT[%s]: update outgoing voice session on channel %d to radio_id %d\n",
				this->name, session->channel_id, session->receiver_radio_id);

			this->direction = outgoing;

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_DIAL);
			break;
		}
		case EV_STOP:
		{
			RadiohytSession *session = find_session_by_channel_id(this->peer_channel_id);
			ast_assert(session != NULL);
			ast_assert(session->owner == this);
			if (session && session->owner == this) {
				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[6] call on channel %d\n",
					this->name, session->channel_id);
				remove_session(this, session);
			}
			radiohyt_chan_ev_stop(this);
			break;
		}
		case EV_ON_BREAK:
			monitor_abort_tport(&this->tport);
			if (this->owner)
				ast_queue_hangup_with_cause(this->owner, AST_CAUSE_CONGESTION);
			break;
		case EV_ON_CHAN_STATE_TIMEOUT:
			monitor_abort_tport(&this->tport);
			radiohyt_chan_ev_timeout(this);
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	case CHAN_RADIOHYT_ST_CANCEL:
		switch (ev) {
		case EV_ON_MAKE_CALL_RESPONSE:
		{
			struct ast_json *result;
			struct ast_json *recv_msg = (struct ast_json *)arg;
			int request_id;
			ENV_ASSERT(recv_msg);

			request_id = ast_json_integer_get(ast_json_object_get(recv_msg, "id"));
			result = ast_json_object_get(recv_msg, "result");
			rc = ast_json_integer_get(ast_json_object_get(result, "Result"));
			if (rc) {
				RadiohytSession *session = find_session_by_request_id(request_id);
				ast_assert(session != NULL);
				ast_assert(session->owner == this);

				//NB: here we are waiting for response on request we sent early, without any checks!
				ast_log(LOG_WARNING, "RADIOHYT[%s]: outgoing voice session on channel %d to radio_id %d is rejected with code: %d\n",
					this->name, session->channel_id, session->receiver_radio_id, rc);

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[4] voice session on channel %d\n",
					this->name, session->channel_id);

				remove_session(this, session);

				radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
				monitor_abort_tport(&this->tport);

				if (this->owner) {
					switch (rc) {
					case Failure:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_NORMAL_TEMPORARY_FAILURE);
						break;
					case InvalidParameter:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_INVALID_MSG_UNSPECIFIED);
						break;
					case RadioBusy:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_USER_BUSY);
						break;
					case InvalidTargetAddress:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_INCOMPATIBLE_DESTINATION);
						break;
					default:
						ast_queue_hangup_with_cause(this->owner, AST_CAUSE_PROTOCOL_ERROR);
						ENV_ASSERT(0);
						break;
					}
				}
			}
			break;
		}
		case EV_ON_CALL_SESSION_EST:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			struct ast_json *msg_params = ast_json_object_create();

			ast_assert(session != NULL);
			ast_assert(session->owner == this);
			ast_assert(this->direction == outgoing);

			ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(this->peer_channel_id));

			radiohyt_chan_send_request(this, HYTERUS_REQUEST_STOP_VOICE_CALL, msg_params, 0);
			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_HANGTIME);
			break;
		}
		case EV_ON_CALL_SESSION_FAIL:
		{
			RadiohytSession *session = (RadiohytSession *)arg;
			ast_assert(session != NULL);

			ast_log(LOG_WARNING, "RADIOHYT[%s]: outgoing voice session on channel %d to radio_id %d is failed\n",
				this->name, session->channel_id, session->receiver_radio_id);

			radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_READY);
			break;
		}
		case EV_MAKE_CALL:
			rc = E_BUSY;
			break;
		case EV_ON_DROP_CALL_RESPONSE:
			break;
		case EV_STOP:
		{
			RadiohytSession *session = find_session_by_channel_id(this->peer_channel_id);
			ast_assert(this->direction == outgoing);

			if (session && session->owner == this) {
				struct ast_json *msg_params = ast_json_object_create();
				ast_json_object_set(msg_params, "ChannelId", ast_json_integer_create(this->peer_channel_id));

				radiohyt_chan_send_request(this, HYTERUS_REQUEST_STOP_VOICE_CALL, msg_params, 0);

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy[6] call on channel %d\n",
					this->name, session->channel_id);

				remove_session(this, session);
			}

			radiohyt_chan_ev_stop(this);
			break;
		}
		case EV_ON_CHAN_STATE_TIMEOUT:
		case EV_ON_REQUEST_TIMEOUT:
			monitor_abort_tport(&this->tport);
			radiohyt_chan_ev_timeout(this);
			break;
		case EV_ON_BREAK:
			break;
		default:
			ast_log(LOG_WARNING, "RADIOHYT[%s]: ignore event\n", this->name);
			return -1;
		}
		break;

	}
	return rc;
}

static void handle_channel_status(Channel *this, struct ast_json *params)
{
	int channel_id = ast_json_integer_get(ast_json_object_get(params, "ChannelId"));
	int online = ast_json_is_true(ast_json_object_get(params, "Online"));

	if (this->on_changed_channel_state_cb)
		this->on_changed_channel_state_cb(this, channel_id, online);
}

static void handle_subscriber_status(Channel *this, struct ast_json *params)
{
	int radio_id = ast_json_integer_get(ast_json_object_get(params, "RadioId"));
	int channel_id = ast_json_integer_get(ast_json_object_get(params, "ChannelId"));
	int online = ast_json_is_true(ast_json_object_get(params, "Online"));

	if (this->on_changed_subscriber_state_cb)
		this->on_changed_subscriber_state_cb(this, radio_id, channel_id, online);
}

static void handle_call_state(Channel *this, struct ast_json *params)
{
	int initiator;
	int chan_id, call_state;
	RadiohytSession *session;
	int need_to_remove_session = 0;

	ast_assert(ast_json_object_get(params, "ChannelId"));
	chan_id = ast_json_integer_get(ast_json_object_get(params, "ChannelId"));

	ast_assert(ast_json_object_get(params, "CallState"));
	call_state = ast_json_integer_get(ast_json_object_get(params, "CallState"));

	if (call_state == CallInitiated) {
		handle_call_session_established(this, params);
		return;
	}

	session = find_session_by_channel_id(chan_id);
	if (session == NULL) {
		ast_debug(1, "RADIOHYT[%s]: can't find session for channel: %d\n", this->name, chan_id);
		return;
	}

	if (session->owner == this) {
		switch (call_state) {
		case CallInitiated:
			radiohyt_chan_fsm(this, EV_ON_CALL_SESSION_EST, session);
			break;
		case CallInHangtime:
			radiohyt_chan_fsm(this, EV_ON_CHANNEL_IN_HANGTIME, session);
			break;
		case CallEnded:
			radiohyt_chan_fsm(this, EV_ON_CHANNEL_IS_FREED, session);
			need_to_remove_session = 1;
			break;
		case NoAck:
		case CallInterrupted:
			radiohyt_chan_fsm(this, EV_ON_CALL_SESSION_FAIL, session);
			need_to_remove_session = 1;
			break;
		}
	} else if (session->owner == NULL) {
		switch (call_state) {
		case CallEnded:
		case NoAck:
		case CallInterrupted:
			ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy voice session on channel %d\n",
				this->name, session->channel_id);
			need_to_remove_session = 1;
			break;
		}
	}

	if (session->status_owner == this || session->status_owner == NULL)
		if (this->on_changed_call_state_cb)
			this->on_changed_call_state_cb(this, session->source, session->receiver_radio_id, call_state);

	if (need_to_remove_session) {
		ast_log(LOG_NOTICE, "RADIOHYT[%s]: destroy voice session on channel %d\n",
			this->name, session->channel_id);
		remove_session(this, session);
	}
}

static void handle_call_session_established(Channel *this, struct ast_json *params)
{
	int initiator, call_type;
	int chan_id, radio_id, call_state;
	char caller_id[HYTERUS_NAME_MAX_LEN];
	RadiohytSession *session = NULL;
	int use_case_num = -1;

	ast_assert(ast_json_object_get(params, "ChannelId"));
	chan_id = ast_json_integer_get(ast_json_object_get(params, "ChannelId"));
	ast_assert(chan_id);

	ast_assert(ast_json_object_get(params, "InitiatorType"));
	initiator = ast_json_integer_get(ast_json_object_get(params, "InitiatorType"));

	ast_assert(ast_json_object_get(params, "CallType"));
	call_type = ast_json_integer_get(ast_json_object_get(params, "CallType"));

	ast_assert(ast_json_object_get(params, "CallState"));
	call_state = ast_json_integer_get(ast_json_object_get(params, "CallState"));

	ast_assert(ast_json_object_get(params, "Source"));
	ast_copy_string(caller_id, ast_json_string_get(ast_json_object_get(params, "Source")), HYTERUS_NAME_MAX_LEN);
	ast_assert(strlen(caller_id));

	ast_assert(ast_json_object_get(params, "ReceiverRadioId"));
	radio_id = ast_json_integer_get(ast_json_object_get(params, "ReceiverRadioId"));
	ast_assert(radio_id);

	/*
	 * Possible situations:
	 
	 #	Initiator	Call_type 	Source		ReceiverRadioId		UseCase
	(1)	1 (dispatcher)	0 (private)	login(own)	radio_id		outgoing call AICS->Radio
	(2)	1		1 (group)	login(own)	radio_id(group)		outgoing call AICS->RadioGroup
	(3)	2 (radio)	0		radio_id	station_id		incoming call AICS<-Radio
	(4)	1		0		login(alien)	radio_id		alien outgoing call to Radio
	(5)	1		1		login(alien)	radio_id(group)		alien outgoing call to RadioGroup
	(6)	2		0		radio_id	station_id		alien incoming call from Radio to Dispatcher (we shouldn't get this call)
	(7)	2		0		radio_id	radio_id		call from Radio to Radio
	(8)	2		1		radio_id	radio_id(group)		call from Radio to Group
	 
	 * UseCases 1,2,3 must be processed into channel fsm as call state notifications
	 * UseCases 4,5,6,7,8 must be processed as channel state notifications
	 */

	// find session information
	session = find_session_by_channel_id(chan_id);

	if (initiator == Dispatcher) {
		// processing of use cases (1)(2)(4)(5)
		if (strcasecmp(caller_id, this->login) == 0) {
			if (call_type == Private) {
				use_case_num = 1;

				ast_assert(session);
				ast_assert(session->owner);

				// Skip notify, if channel is not owner of radiochannel's status
				if (session->owner != this) {
					ast_debug(1, "RADIOHYT[%s]: is not owner(%s) of session on channel %d\n",
						this->name, ((Channel *)session->owner)->name, session->channel_id);
					return;
				}

			} else if (call_type == Group) {
				use_case_num = 2;

				ast_assert(session);
				ast_assert(session->owner);

				// Skip notify, if channel is not owner of radiochannel's status
				if (session->owner != this) {
					ast_debug(1, "RADIOHYT[%s]: is not owner(%s) of session on channel %d\n",
						this->name, ((Channel *)session->owner)->name, session->channel_id);
					return;
				}

			} else {
				ast_assert(0);
				return;
			}
			
			// processing of use cases (1)(2)
			if (this->state == CHAN_RADIOHYT_ST_RING ||
			    this->state == CHAN_RADIOHYT_ST_CANCEL ||
			    this->state == CHAN_RADIOHYT_ST_HANGTIME) {
				strcpy(session->source, caller_id);
				session->receiver_radio_id = radio_id;
				session->direction = outgoing;

				ast_log(LOG_NOTICE, "RADIOHYT[%s]: update[3] call on channel %d to radio_id %d\n",
					this->name, session->channel_id, session->receiver_radio_id);

				radiohyt_chan_fsm(this, EV_ON_CALL_SESSION_EST, session);
			} else {
			    ast_assert(0);
			}
			
		} else {
			if (call_type == Private) {
				use_case_num = 4;

			} else if (call_type == Group) {
				use_case_num = 5;

			} else {
				ast_assert(0);
				return;
			}
			// processing of use cases (4)(5)
			if (session == NULL) {
			    if (this->policy == STATIC || (this->policy == DYNAMIC && !strcmp(this->name, "channel0"))) {
				session = (RadiohytSession *)ast_calloc(1, sizeof(*session));
				session->owner = NULL;
				session->status_owner = this;
				session->caller_id[0] = '\0';
				strcpy(session->source, caller_id);
				session->receiver_radio_id = radio_id;
				session->channel_id = chan_id;
				session->request_id = 0;
				session->direction = external;
				AST_LIST_INSERT_TAIL(&radiohyt_session_queue, session, next);
			    } else {
				// non-root dynamic channel skips this notify
				ast_debug(1, "RADIOHYT[%s]: skip call state notification\n", this->name);
				return;
			    }
			}
		}

	} else if (initiator == Subscriber) {
		// processing next Use Cases:
		//(3)(6) radio_id -> station_id
		//(7) radio_id -> radio_id
		//(8) radio_id -> group_id
		
		// todo: we need to distinct use cases
		use_case_num = 3;

		// Channel is not owner of radio channel
		if (session && session->status_owner && session->status_owner != this)
			return;

		if (session == NULL) {
		    // non-root dynamic channel should skip call state notification
		    if (this->policy == DYNAMIC && strcmp(this->name, "channel0")) {
			ast_debug(1, "RADIOHYT[%s]: skip call state notification\n", this->name);
			return;
		    }
		    if (this->state == CHAN_RADIOHYT_ST_READY) {
			// store session information
			session = (RadiohytSession *)ast_calloc(1, sizeof(*session));
			session->owner = this;		// may be cleared later inside radiohyt_chan_fsm()
			session->status_owner = this;	// may be cleared later inside radiohyt_chan_fsm()
			session->caller_id[0] = '\0';
			strcpy(session->source, caller_id);
			session->receiver_radio_id = radio_id;
			session->channel_id = chan_id;
			session->request_id = 0;
			session->direction = incoming;
			AST_LIST_INSERT_TAIL(&radiohyt_session_queue, session, next);
		    } else {
			// store session information
			session = (RadiohytSession *)ast_calloc(1, sizeof(*session));
			session->owner = NULL;
			session->status_owner = NULL;
			session->caller_id[0] = '\0';
			strcpy(session->source, caller_id);
			session->receiver_radio_id = radio_id;
			session->channel_id = chan_id;
			session->request_id = 0;
			session->direction = incoming;
			AST_LIST_INSERT_TAIL(&radiohyt_session_queue, session, next);

			ast_log(LOG_NOTICE, "RADIOHYT[%s]: store call on channel %d to radio_id %d\n",
				this->name, session->channel_id, session->receiver_radio_id);
		    }
		} else {
			// Channel can try to take an owner of session
			if (!(session->owner || session->status_owner)) {
				if (this->policy == STATIC && this->state == CHAN_RADIOHYT_ST_READY) {
					session->owner = this;
					session->status_owner = this;
					ast_log(LOG_NOTICE, "RADIOHYT[%s]: try to own call on channel %d to radio_id %d\n",
						this->name, session->channel_id, session->receiver_radio_id);
				} else {
					ast_debug(1, "RADIOHYT[%s]: skip call state notification\n", this->name);
					return;
				}
			} else {
				// Channel can receive incoming notification for early outgoing call
				strcpy(session->source, caller_id);
				session->receiver_radio_id = radio_id;
				session->direction = incoming;
			}
		}

		if (session->owner == this) {
			ast_assert((this->state == CHAN_RADIOHYT_ST_READY || 
				    this->state == CHAN_RADIOHYT_ST_CANCEL ||
				    this->state == CHAN_RADIOHYT_ST_HANGTIME));
			radiohyt_chan_fsm(this, EV_ON_CALL_SESSION_EST, session);
		}
		
		if (session->owner == NULL)
			session->direction = external;
		
	} else {
		ast_assert(0);
		return;
	}

	ast_assert(use_case_num != -1);

	ast_assert(session != NULL);
	if (session->status_owner && session->status_owner != this)
		return;

	if (this->on_changed_call_state_cb)
		this->on_changed_call_state_cb(this, caller_id, radio_id, call_state);
}

/*
 * internal functions
 */
static int radiohyt_chan_start(void *arg)
{
	Channel *this = (Channel *)arg;

	ENV_THREAD_INIT_CHECKER(monitor_thread);

	return radiohyt_chan_fsm(this, EV_START, NULL);
}

static int radiohyt_chan_stop(void *arg)
{
	Channel *this = (Channel *)arg;

	radiohyt_chan_fsm(this, EV_STOP, NULL);

	ENV_THREAD_STOP_CHECKER(monitor_thread);
	ENV_THREAD_RESET_CHECKER(monitor_thread);

	return E_OK;
}

static int radiohyt_chan_make_call(void *arg)
{
	struct chan_make_call_function_arg *farg = (struct chan_make_call_function_arg *)arg;
	Channel *chan = farg->chan;

	return radiohyt_chan_fsm(chan, EV_MAKE_CALL, arg);
}

static int radiohyt_chan_drop_call(void *arg)
{
	struct chan_make_call_function_arg *farg = (struct chan_make_call_function_arg *)arg;
	Channel *chan = farg->chan;

	radiohyt_chan_fsm(chan, EV_DROP_CALL, NULL);
	return E_OK;
}

static int radiohyt_chan_check_radio(void *arg)
{
	struct chan_check_radio_function_arg *farg = (struct chan_check_radio_function_arg *)arg;
	Channel *chan = farg->chan;

	radiohyt_chan_fsm(chan, EV_CHECK_RADIO, arg);
	return E_OK;
}

static int radiohyt_chan_request_timeout(void *arg)
{
	Channel *chan = (Channel *)arg;
	return radiohyt_chan_fsm(chan, EV_ON_REQUEST_TIMEOUT, NULL);
}

/*
 * Transport functions
 */
static int radiohyt_tport_connect(void *arg)
{
	Channel *this = (Channel *)arg;

	// initializing TCP transport
	if (this->tport.fd < 0) {
		int reuse = 1;
		socklen_t len = sizeof(reuse);

		this->tport.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
		if (this->tport.fd < 0) {
			ast_log(LOG_ERROR, "RADIOHYT: tcp: can't create socket: %s\n", strerror(errno));
			return E_FAIL;
		}

		setsockopt(this->tport.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, len);
		reuse = 0;
		if (!getsockopt(this->tport.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, &len)) {
			if (len == sizeof(reuse)) {
				ast_log(LOG_NOTICE, "RADIOHYT: tcp: re-use socket: %d\n", reuse);
			}
		}

		if (!ast_sockaddr_isnull(&this->addr)) {
			ast_log(LOG_NOTICE, "RADIOHYT: tcp: trying to use local address: %s\n", ast_sockaddr_stringify(&this->addr));
			if (ast_bind(this->tport.fd, &this->addr) < 0) {
				ast_log(LOG_WARNING, "RADIOHYT: tcp: can't bind address: %s: %s\n",
					ast_sockaddr_stringify(&this->addr),
					strerror(errno));
				ast_sockaddr_setnull(&this->addr);
			}
		}
	}

	// connecting
	if (ast_connect(this->tport.fd, &this->srv_addr) < 0) {
		if (!(errno == EINPROGRESS || errno == EALREADY)) {
			ast_log(LOG_ERROR, "RADIOHYT: tcp: can't initial connect to %s: %s\n",
				ast_sockaddr_stringify(&this->srv_addr),
				strerror(errno));
			return E_FAIL;
		}
	}
	ast_log(LOG_NOTICE, "RADIOHYT: tcp: connecting to: %s\n", ast_sockaddr_stringify(&this->srv_addr));
	// re-new local address
	ast_getsockname(this->tport.fd, &this->addr);

	// pass io callbacks to monitor
	{
		this->tport.io_mask = AST_IO_OUT;

		this->tport.th_ready_write  = radiohyt_tport_ready_write;
		this->tport.th_ready_read   = NULL;//radiohyt_tport_ready_read;
		this->tport.th_fin_received = NULL;//radiohyt_tport_fin_received;
		this->tport.th_error        = NULL;//radiohyt_tport_error;

		if (monitor_append_tport(&this->tport)) {
			ast_log(LOG_ERROR, "RADIOHYT: tcp: can't append transport into monitor\n");
			radiohyt_tport_close(&this->tport);
			ENV_ASSERT(0);
			return E_FAIL;
		}
	}

#ifdef USE_RTP_ENGINE
	if (this->rtpi) {
		struct ast_sockaddr rtp_addr;

		ast_rtp_instance_get_local_address(this->rtpi, &rtp_addr);
		ast_log(LOG_NOTICE, "RADIOHYT: rtp: using local address: %s\n", ast_sockaddr_stringify(&rtp_addr));

		ast_rtp_instance_activate(this->rtpi);
	}
#else
	// initializing RTP transport
	if (this->rtp_tport.fd < 0) {
		struct ast_sockaddr rtp_addr;

		this->rtp_tport.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (this->rtp_tport.fd < 0) {
			ast_log(LOG_ERROR, "RADIOHYT: rtp: can't create socket: %s\n", strerror(errno));
			return E_FAIL;
		}
		//todo: use range of rtp ports
		ast_sockaddr_copy(&rtp_addr, &this->addr);
		ast_sockaddr_set_port(&rtp_addr, this->rtp_port);
		if (ast_bind(this->rtp_tport.fd, &rtp_addr) < 0) {
			ast_log(LOG_ERROR, "RADIOHYT: rtp: can't bind address: %s: %s\n",
				ast_sockaddr_stringify(&rtp_addr),
				strerror(errno));
			return E_FAIL;
		}
	}
#endif
	return E_OK;
}

static int radiohyt_tport_disconnect(void *arg)
{
	Channel *this = (Channel *)arg;

#ifdef USE_RTP_ENGINE
	if (this->rtpi)
		ast_rtp_instance_stop(this->rtpi);
#else
	// close rtp connection if exist
	if (this->rtp_tport.io_id) {
		monitor_remove_tport(&this->rtp_tport);
	}
	if (this->rtp_tport.fd >= 0) {
		radiohyt_tport_close(&this->rtp_tport);
	}
#endif

	// close tcp connection if exist
	if (this->tport.io_id) {
		monitor_remove_tport(&this->tport);
	}
	if (this->tport.fd >= 0) {
		radiohyt_tport_close(&this->tport);
		if (this->state != CHAN_RADIOHYT_ST_TRYC)
			gettimeofday(&this->tport.stat.disconnection_ts, 0);
	}

	return E_OK;
}

static void radiohyt_tport_close(Transport *tport)
{
	ast_debug(1, "RADIOHYT: close fd: %d\n", tport->fd);
	close(tport->fd);
	tport->fd = -1;
}

static inline int radiohyt_chan_load_subscribers(void *arg)
{
	struct chan_get_function_arg *farg = (struct chan_get_function_arg *)arg;
	Channel *this = farg->chan;

	if (this->state < CHAN_RADIOHYT_ST_READY)
		return E_FAIL;

	radiohyt_chan_send_request(this, HYTERUS_REQUEST_GET_SUBSCRIBERS, NULL, farg->option);
	return E_OK;
}

static inline int radiohyt_chan_load_groups(void *arg)
{
	struct chan_get_function_arg *farg = (struct chan_get_function_arg *)arg;
	Channel *this = farg->chan;

	if (this->state < CHAN_RADIOHYT_ST_READY)
		return E_FAIL;

	radiohyt_chan_send_request(this, HYTERUS_REQUEST_GET_GROUPS, NULL, farg->option);
	return E_OK;
}

static inline int radiohyt_chan_load_stations(void *arg)
{
	struct chan_get_function_arg *farg = (struct chan_get_function_arg *)arg;
	Channel *this = farg->chan;

	if (this->state < CHAN_RADIOHYT_ST_READY)
		return E_FAIL;

	radiohyt_chan_send_request(this, HYTERUS_REQUEST_GET_CHANNELS, NULL, farg->option);
	return E_OK;
}

/*
 * Processing FSM events
 */

#ifdef TEST_FRAMEWORK
static int radiohyt_chan_fake_on_recv(void *arg)
{
	struct chan_fake_recv_function_arg *farg = (struct chan_fake_recv_function_arg *)arg;
	radiohyt_chan_ev_on_recv(farg->chan, farg->msg);
	return E_OK;
}

static int radiohyt_chan_fake_on_break(void *arg)
{
	struct chan_fake_recv_function_arg *farg = (struct chan_fake_recv_function_arg *)arg;
	radiohyt_chan_ev_on_break(farg->chan);
	return E_OK;
}

static int radiohyt_chan_fake_on_connect(void *arg)
{
	struct chan_fake_recv_function_arg *farg = (struct chan_fake_recv_function_arg *)arg;
	radiohyt_chan_fsm(farg->chan, EV_ON_CONNECT, NULL);
	return E_OK;
}
#endif

static void radiohyt_chan_ev_on_recv(Channel *this, struct ast_json *arg)
{
	RAII_VAR(struct ast_json *, msg, arg, ast_json_unref);

	struct ast_json *method = ast_json_object_get(msg, "method");
	struct ast_json *params = ast_json_object_get(msg, "params");
	struct ast_json *result = ast_json_object_get(msg, "result");
	struct ast_json *id     = ast_json_object_get(msg, "id");

	ENV_THREAD_CHECK(monitor_thread);

	if (this->debug && msg) {
		char *data = ast_json_dump_string_format(msg, AST_JSON_PRETTY);
#ifndef TEST_FRAMEWORK
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: tcp: receive from: %s\n%s\n\n",
			this->name, ast_sockaddr_stringify(&this->srv_addr), data);
#else
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: fake: receive:\n%s\n\n",
			this->name, data);
#endif
		ast_json_free(data);
	}

	if (result) {
		int resp_cmd_res = ast_json_integer_get(ast_json_object_get(result, "Result"));
		int resp_transaction_id = -1;
		RadiohytRequest *request = NULL, *item = NULL;

		if (!id) {
			ast_log(LOG_ERROR, "RADIOHYT[%s]: receive response without id\n", this->name);
			ast_assert(0);
			return;
		}
		resp_transaction_id = ast_json_integer_get(id);

		AST_LIST_TRAVERSE_SAFE_BEGIN(&this->request_queue, item, next) {
			if (item->transaction_id == resp_transaction_id) {
				request = item;
				AST_LIST_REMOVE_CURRENT(next);
				break;
			}
		}
		AST_LIST_TRAVERSE_SAFE_END;

		if (!request) {
			ast_log(LOG_ERROR, "RADIOHYT[%s]: received response with unknown id: %d\n",
				this->name, resp_transaction_id);
			return;
		}

		if (this->debug)
			ast_log(LOG_VERBOSE, "RADIOHYT[%s]: reset request timer(%d)\n",
				this->name, request->sched_id);

		AST_SCHED_DEL(monitor_sched_ctx(), request->sched_id);

		switch (request->method) {
		case HYTERUS_REQUEST_GET_CHANNELS:
			if (!resp_cmd_res) {
				//todo: pass option
				if (this->on_receive_station_list_cb)
					this->on_receive_station_list_cb(this, 0, ast_json_object_get(result, "Channels"), request->magic);
			} else {
				ast_log(LOG_ERROR, "RADIOHYT[%s]: receive erroneous response on request station list: %d\n",
					this->name, resp_cmd_res);
			}
			break;

		case HYTERUS_REQUEST_GET_GROUPS:
			if (!resp_cmd_res) {
				//todo: pass option
				if (this->on_receive_group_list_cb)
					this->on_receive_group_list_cb(this, 0, ast_json_object_get(result, "CallGroups"), request->magic);
			} else {
				ast_log(LOG_ERROR, "RADIOHYT[%s]: receive erroneous response on request group list: %d\n",
					this->name, resp_cmd_res);
			}
			break;

		case HYTERUS_REQUEST_GET_SUBSCRIBERS:
			if (!resp_cmd_res) {
				//todo: pass option
				if (this->on_receive_subscriber_list_cb)
					this->on_receive_subscriber_list_cb(this, 0, ast_json_object_get(result, "Subscribers"), request->magic);
			} else {
				ast_log(LOG_ERROR, "RADIOHYT[%s]: receive erroneous response on request subscriber list: %d\n",
					this->name, resp_cmd_res);
			}
			break;

		case HYTERUS_REQUEST_START_VOICE_CALL:
			radiohyt_chan_fsm(this, EV_ON_MAKE_CALL_RESPONSE, msg);
			break;

		case HYTERUS_REQUEST_STOP_VOICE_CALL:
			radiohyt_chan_fsm(this, EV_ON_DROP_CALL_RESPONSE, msg);
			break;

		case HYTERUS_REQUEST_START_GROUP_CALL:
		case HYTERUS_REQUEST_START_REMOTE_MONITOR:
		case HYTERUS_REQUEST_CHECK_RADIO:
		case HYTERUS_REQUEST_PING:
			break;

		default:
			ast_log(LOG_ERROR, "RADIOHYT[%s]: receive unexpected response\n", this->name);
		}
		ast_free(request);

	} else if (method && params) {
		const char *str_method = ast_json_string_get(method);

		if (!strcasecmp(str_method, hyterus_msg_type_to_str(HYTERUS_REQUEST_AUTHENTICATE))) {
			radiohyt_chan_fsm(this, EV_ON_AUTHENTICATION_REQUEST, msg);
		} else if (!strcasecmp(str_method, hyterus_msg_type_to_str(HYTERUS_NOTIFY_AUTH_STATUS))) {
			radiohyt_chan_fsm(this, EV_ON_AUTHENTICATION_RESPONSE, msg);
		} else if (!strcasecmp(str_method, hyterus_msg_type_to_str(HYTERUS_NOTIFY_CHANNEL_STATUS))) {
			handle_channel_status(this, params);
		} else if (!strcasecmp(str_method, hyterus_msg_type_to_str(HYTERUS_NOTIFY_SUBSCRIBER_STATUS))) {
			handle_subscriber_status(this, params);
		} else if (!strcasecmp(str_method, hyterus_msg_type_to_str(HYTERUS_NOTIFY_CALL_STATE))) {
			handle_call_state(this, params);
		} else {
			ast_log(LOG_ERROR, "RADIOHYT[%s]: receive unsupported method: %s\n", this->name, str_method);
			radiohyt_chan_send_response(this, ast_json_integer_get(id), NULL);
		}
	} else {
		ast_log(LOG_ERROR, "RADIOHYT[%s]: receive unknown message\n", this->name);
	}
}

static void radiohyt_chan_ev_on_break(Channel *this)
{
	clean_state_timers(this);
	clean_active_requests(this);
	clean_active_sessions(this);

	radiohyt_chan_fsm(this, EV_ON_BREAK, NULL);
#ifndef TEST_FRAMEWORK
	radiohyt_tport_disconnect(this);

	radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_TRYC);

	if (this->on_changed_connection_state_cb)
		this->on_changed_connection_state_cb(this, this->state);

	SET_TIMER("connect", sched_conn_id, connect_timeout, radiohyt_connect_timeout_cb);
#else
	radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_TRYC);

	if (this->on_changed_connection_state_cb)
		this->on_changed_connection_state_cb(this, this->state);
#endif
}

static void radiohyt_chan_ev_timeout(Channel *this)
{
	clean_active_sessions(this);
#ifndef TEST_FRAMEWORK
	radiohyt_tport_disconnect(this);

	radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_TRYC);

	if (this->on_changed_connection_state_cb)
		this->on_changed_connection_state_cb(this, this->state);

	SET_TIMER("connect", sched_conn_id, connect_timeout, radiohyt_connect_timeout_cb);
#else
	radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_IDLE);
#endif
}

static void radiohyt_chan_ev_stop(Channel *this)
{
	RESET_TIMER(sched_conn_id);

	clean_state_timers(this);
	clean_active_requests(this);
	clean_active_sessions(this);
#ifndef TEST_FRAMEWORK
	radiohyt_tport_disconnect(this);
#endif
	radiohyt_chan_switch_to_state(this, CHAN_RADIOHYT_ST_IDLE);
}

/*
 * TCP transport callbacks
 */
static int radiohyt_tport_ready_write(Transport *tport)
{
	Channel *chan = (Channel *)(tport->owner);
	int err = 0;
	socklen_t len = sizeof(int);

	getsockopt(tport->fd, SOL_SOCKET, SO_ERROR, &err, &len);
	if (err == 0) {
		ENV_DVAR(int rc);

		ENV_DVAR(rc = )ast_getsockname(tport->fd, &chan->addr);
		ENV_ASSERT(rc == 0);

		tport->io_mask = AST_IO_IN | AST_IO_HUP | AST_IO_ERR;

		tport->th_ready_write  = NULL;
		tport->th_ready_read   = radiohyt_tport_ready_read;
		tport->th_fin_received = radiohyt_tport_fin_received;
		tport->th_error        = radiohyt_tport_error;

		ENV_DVAR(rc = )monitor_append_tport(tport);
		ENV_ASSERT(rc == E_OK);

		ast_log(LOG_NOTICE, "RADIOHYT[%s]: is successfully connected\n", chan->name);

		radiohyt_chan_fsm(chan, EV_ON_CONNECT, NULL);
	} else {
		tport->th_ready_write  = NULL;

		ast_log(LOG_WARNING, "RADIOHYT[%s]: can't connect to %s: %s\n", chan->name,
			ast_sockaddr_stringify(&chan->srv_addr),
			strerror(err));

		radiohyt_chan_fsm(chan, EV_ON_CONNECT_FAIL, NULL);
	}

	return TPORT_RC_CONTINUE;
}

static int radiohyt_tport_ready_read(Transport *tport)
{
	Channel *chan = (Channel *)(tport->owner);
	int ix, read_bytes, eat_bytes, rest_bytes;

	ENV_ASSERT(tport->fd >= 0);

	memset(tport->data, 0, TCP_BUFSIZE);
	read_bytes = read(tport->fd, tport->data, TCP_BUFSIZE);
	if (read_bytes > 0) {
		gettimeofday(&tport->stat.last_recv_ts, 0);

		rest_bytes = read_bytes;
		for (ix = 0; rest_bytes;) {
			int rc = eat_buffer(&chan->parser, &tport->data[ix], read_bytes-ix, &eat_bytes);
			ast_debug(2, "RADIOHYT[%s]: eaten %d bytes from received %d\n",
				chan->name, eat_bytes, rest_bytes);

			rest_bytes -= eat_bytes;
			ix += eat_bytes;

			if (rc == E_OK) {
				struct ast_json *msg = decoded_msg(&chan->parser);
				radiohyt_chan_ev_on_recv(chan, msg);
				++tport->stat.recv_msg_cnt;
			} else if (rc == E_IN_PROGRESS) {
				break;
			}
		}
	} else if (read_bytes == 0 || (read_bytes < 0 && errno != EAGAIN)) {
		ast_log(LOG_ERROR, "RADIOHYT[%s]: recv error: %s\n", chan->name, strerror(errno));
		radiohyt_chan_ev_on_break(chan);
		return TPORT_RC_BREAK;
	}
	return TPORT_RC_CONTINUE;
}

static void radiohyt_tport_fin_received(Transport *tport)
{
	radiohyt_chan_ev_on_break((Channel *)(tport->owner));
}

static void radiohyt_tport_error(Transport *tport, int rc)
{
	radiohyt_chan_ev_on_break((Channel *)(tport->owner));
}

#ifndef USE_RTP_ENGINE
/*
 * RTP transport
 */
static int radiohyt_rtp_tport_ready_write(Transport *tport, struct ast_frame *f)
{
/*
	static int cnt = 0;
	if (++cnt < 10) {
		ast_log(LOG_WARNING, "RADIOHYT: channel: rtp: send frame format: %s\n",
			ast_getformatname(&f->subclass.format));
	}
*/
	/*int rc = */send(tport->fd, f->data.ptr, f->datalen, 0);

	gettimeofday(&tport->stat.last_send_ts, 0);
	++tport->stat.send_msg_cnt;

	return TPORT_RC_CONTINUE;
}

static int radiohyt_rtp_tport_ready_read(Transport *tport)
{
	unsigned len = 160;
	char data[len];
	Channel *chan = (Channel *)(tport->owner);

	ENV_ASSERT(tport->fd >= 0);
	for (;;) {
		ssize_t rc = read(tport->fd, data, len);
		if (rc > 0) {
			//struct ast_channel *peer = ast_channel_bridge_peer(chan->owner);

			static unsigned seqno = 1;
			struct ast_frame f = {
				.frametype = AST_FRAME_VOICE,
				.datalen = rc,
				.samples = rc,
				.mallocd = 0,
				.offset = 0,
				.src = __PRETTY_FUNCTION__,
				.seqno = seqno++,
				.len = 20	// ms
			};

			gettimeofday(&tport->stat.last_recv_ts, 0);
			++tport->stat.recv_msg_cnt;

			f.delivery = tport->stat.last_recv_ts;
			f.data.ptr = data;

			// FIXME
			//f.subclass.format = ast_format_ulaw;
			ast_getformatbyname("ulaw", &f.subclass.format);

			//if (peer) {
				ast_write(chan->peer, &f);
			//}
		} else {
			if (rc == 0 || (rc < 0 && errno != EAGAIN)) {
				ast_log(LOG_ERROR, "RADIOHYT[%s]: rtp: recv error: %s\n", chan->name, strerror(errno));
				return TPORT_RC_BREAK;
			}
			break;
		}
	}
	return TPORT_RC_CONTINUE;
}

static void radiohyt_rtp_tport_fin_received(Transport *tport)
{
	ast_log(LOG_ERROR, "RADIOHYT: rtp: tport: fin\n");
}

static void radiohyt_rtp_tport_error(Transport *tport, int rc)
{
	ast_log(LOG_ERROR, "RADIOHYT: rtp: tport error: %s(%d)\n", strerror(rc), rc);
}
#endif

/*
 *
 */
static const char *channel_event_str[] =
{
	"unknown",
	"start",
	"on_connect",
	"on_connect_fail",
	"on_break",
	"on_auth_request",
	"on_auth_response",
	"on_request_timeout",
	"on_chan_state_timeout",
	"make_call",
	"drop_call",
	"check_radio",
	"on_make_call_response",
	"on_drop_call_response",
	"on_call_established",
	"on_call_failed",
	"on_channel_in_hangtime",
	"on_channel_is_freed",
	"stop",
};

static const char *channel_state_str[] =
{
	"idle",
	"connecting",
	"connected",
	"wait_for_auth",
	"ready",
	"dialing",
	"ringing",
	"busy",
	"detaching",
	"detaching_2",
	"hangtime",
	"cancel",
};

ENV_COMPILE_ASSERT(sizeof(channel_event_str)/sizeof(*channel_event_str) == CHAN_RADIOHYT_EV_COUNT, ce_str_err);
ENV_COMPILE_ASSERT(sizeof(channel_state_str)/sizeof(*channel_state_str) == CHAN_RADIOHYT_ST_COUNT, cs_str_err);

const char *channel_event_to_str(int event)
{
	if (event < CHAN_RADIOHYT_EV_COUNT)
		return channel_event_str[event];
	ENV_ASSERT(0);
	return "error";
}

const char *channel_state_to_str(int state)
{
	if (state < CHAN_RADIOHYT_ST_COUNT)
		return channel_state_str[state];
	ENV_ASSERT(0);
	return "error";
}

#ifdef TEST_FRAMEWORK
static int radiohyt_chan_session_get(void *arg)
{
	struct chan_session_get_function_arg *farg = (struct chan_session_get_function_arg *)arg;
	RadiohytSession *session = NULL;

	do {
		if (farg->channel_id != -1)
			session = find_session_by_channel_id(farg->channel_id);
		if (session)
			break;
		return E_NOT_FOUND;
	} while(0);

	if (farg->session)
		memcpy(farg->session, session, sizeof(*session));
	return E_OK;
}
#endif

#ifndef IACS_RADIOHYT_MONITOR_H
#define IACS_RADIOHYT_MONITOR_H

#include <sys/time.h>

#include "asterisk/linkedlists.h"
#include "asterisk/json.h"

#include "env_debug.h"

#define TPORT_RC_BREAK 0
#define TPORT_RC_CONTINUE 1

#define TCP_SEND_TIMEOUT_USEC 2000000
#define TCP_BUFSIZE 65536

typedef struct transport_s Transport;
typedef struct statistics_s Statistics;

struct statistics_s {
	/*! Timestamp of creation event */
	struct timeval creation_ts;
	/*! Timestamp of connection event */
	struct timeval connection_ts;
	/*! Timestamp of disconnection event */
	struct timeval disconnection_ts;

	/*! Timestamp of last received message from the peer */
	struct timeval last_recv_ts;
	/*! Timestamp of last sent message to the peer */
	struct timeval last_send_ts;

	/*! Counter of send messages */
	unsigned long send_msg_cnt;
	/*! Counter of recv messages */
	unsigned long recv_msg_cnt;

	/*! Connection's counter */
	unsigned connections_cnt;
	/*! Call's counter */
	unsigned calls_cnt;

	/*! Timestamp of channel request event */
	struct timeval voice_request_ts;
	/*! Timestamp of channel answer event */
	struct timeval voice_request_answer_ts;

	/*! Answer time statistics */
	int min_answer_time_ms;
	int max_answer_time_ms;
	int ave_answer_time_ms;
	uint64_t sum_answer_time_ms;
};

/* Some transport */
struct transport_s {
	int fd;			/* File Descriptor (usually socket) */

	int io_mask;		/* Asterisk IO Mask */
	int  *io_id;		/* Return value of ast_io_add() */
	void *owner;		/* Used to store pointer to chan_radiohyt_pvt */

	char data[TCP_BUFSIZE]; /* receiving buffer */

	Statistics stat;	/* Network statistics */

	int  (*th_ready_read)(Transport *);	/* read callback */
	int  (*th_ready_write)(Transport *);	/* write callback */
	void (*th_fin_received)(Transport *);	/* connection closed callback */
	void (*th_error)(Transport *, int rc); 	/* connection error callback (ICMP error) */

	AST_LIST_ENTRY(transport_s) entry;
};

/*! callback is called when connection with server changes state */
typedef void (*Change_connection_state_callback)(void *this, int state);
/*! callback takes pointer to JSON if rc equals to 0 */
typedef void (*Receive_stations_callback)(void *this, int rc, struct ast_json *, int option);
/*! callback takes pointer to JSON if rc equals to 0 */
typedef void (*Receive_groups_callback)(void *this, int rc, struct ast_json *, int option);
/*! callback takes pointer to JSON if rc equals to 0 */
typedef void (*Receive_subscribers_callback)(void *this, int rc, struct ast_json *, int option);
/*! callback is called when incoming call comes in */
typedef int  (*Incoming_call_callback)(void *this, const struct ast_sockaddr *addr, const char *caller_id, int radio_id, int chan_id, char *sip_id);
/*! callback is called when state of call is changed */
typedef void (*Change_call_state_callback)(void *this, const char *caller_id, int radio_id, int state);
/*! callback is called when state of channel is changed */
typedef void (*Change_channel_state_callback)(void *this, int channel_id, int online);
/*! callback is called when state of subscriber is changed */
typedef void (*Change_subscriber_state_callback)(void *this, int radio_id, int channel_id, int online);

/*! Monitor interface for module */
extern int  monitor_start(void);
extern void monitor_stop(void);
/*! Monitor interface for channel */
extern int  monitor_append_tport(Transport *tport);
extern void monitor_remove_tport(Transport *tport);
extern void monitor_abort_tport(Transport *tport);
/*! Monitor interface for both module and channel */
extern int  monitor_synch_task(Transport *tport, int (*user_func)(void*), void *arg);
extern struct ast_sched_context *monitor_sched_ctx(void);

#endif


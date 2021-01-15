#ifndef IACS_RADIOHYT_CHAN_RADIOHYT_H
#define IACS_RADIOHYT_CHAN_RADIOHYT_H

#include "monitor.h"
#include "msg_parser.h"

typedef enum channel_event_s {
	EV_UNKNOWN = 0,
	EV_START,
	EV_ON_CONNECT,
	EV_ON_CONNECT_FAIL,
	EV_ON_BREAK,
	EV_ON_AUTHENTICATION_REQUEST,
	EV_ON_AUTHENTICATION_RESPONSE,
	EV_ON_REQUEST_TIMEOUT,		/*!< response on our request is not received during timeout */
	EV_ON_CHAN_STATE_TIMEOUT,	/*!< channel stay into current state too long */
	EV_MAKE_CALL,			/*!< make call to radio subscriber/callgroup */
	EV_DROP_CALL,			/*!< drop exists call or cancel outgoing call, apply only for outgoing call! */
	EV_CHECK_RADIO,			/*!< check status of radio subscriber */
	EV_ON_MAKE_CALL_RESPONSE,	/*!< receive response on our outgoing start call request */
	EV_ON_DROP_CALL_RESPONSE,	/*!< receive response on our outgoing stop call request */
	EV_ON_CALL_SESSION_EST,		/*!< outgoing/incoming call session is established */
	EV_ON_CALL_SESSION_FAIL,	/*!< outgoing call session can not be established */
	EV_ON_CHANNEL_IN_HANGTIME,	/*!< radio channel is in hangtime */
	EV_ON_CHANNEL_IS_FREED,		/*!< radio channel is freed */
	EV_STOP,

}	channel_event;

#define CHAN_RADIOHYT_EV_COUNT EV_STOP+1

const char *channel_event_to_str(int event);

typedef enum channel_state_s {
	CHAN_RADIOHYT_ST_IDLE = 0,	/*!< initial state after creation or cancellation */
	CHAN_RADIOHYT_ST_TRYC,		/*!< connecting to Hyterus API Server */
	CHAN_RADIOHYT_ST_CONN,		/*!< successfully connected to Hyterus API Server */
	CHAN_RADIOHYT_ST_WFA,		/*!< wait for authentication */
	CHAN_RADIOHYT_ST_READY,		/*!< ready for make outgoing call or accept incoming call */
	CHAN_RADIOHYT_ST_DIAL,		/*!< dialing or sent another request */
	CHAN_RADIOHYT_ST_RING,		/*!< wait for answer on outgoing call */
	CHAN_RADIOHYT_ST_BUSY,		/*!< channel is busy */
	CHAN_RADIOHYT_ST_DETACH,	/*!< channel is now detaching from radio, waiting for response */
	CHAN_RADIOHYT_ST_DETACH_2,	/*!< channel is now detaching from radio, waiting for hangtime notify */
	CHAN_RADIOHYT_ST_HANGTIME,	/*!< wait for radio channel will be finally freed */
	CHAN_RADIOHYT_ST_CANCEL,	/*!< wait for receive response on outgoing call to cancel it */

}	channel_state;

#define CHAN_RADIOHYT_ST_COUNT CHAN_RADIOHYT_ST_CANCEL+1

const char *channel_state_to_str(int);

struct radiohyt_channel_state {
	/*! Current state */
	int state;
	/*! Timeout for current state */
	int timeout;
	/*! Active timer for current state */
	int timer_id;
	/*! Timestamp of switching to this state */
	struct timeval timestamp;
};

typedef struct radiohyt_channel_state RadiohytChannelState;

// Direction of call for our channel
enum call_direction {
	outgoing = 0,
	incoming = 1,
	external = 2
};

// Radiohyt request's information
struct radiohyt_request {
	/*! Used to store Channel pointer */
	void *this;
	/*! Sent transaction identifier */
	int transaction_id;
	/*! Sent method */
	int method;
	/*! Sheduler Item ID for task */
	int sched_id;
	/*! Used to store some data */
	int magic;

	AST_LIST_ENTRY(radiohyt_request) next;
};

typedef struct radiohyt_request RadiohytRequest;

// Radiohyt session's information
struct radiohyt_session {
	/*! Channel - owner of this call session, processing voice */
	void *owner;
	/*! Channel - owner of this call session's status, processing only status. If owner isn't NULL, must be equivalent its. */
	void *status_owner;
	/*! Caller number */
	char caller_id[AST_MAX_EXTENSION];
	/*! Source identifier */
	char source[HYTERUS_NAME_MAX_LEN+1];
	/*! Receiver radio identifier */
	int receiver_radio_id;
	/*! Channel identifier */
	int channel_id;
	/*! Request identifier for outgoing call */
	int request_id;
	/*! Direction of active call */
	int direction;

	AST_LIST_ENTRY(radiohyt_session) next;
};

typedef struct radiohyt_session RadiohytSession;

/*
 * In fact, strategy of trunk behavior with a lack of capacity to make an outgoing call
 */
enum channel_policy {
	STATIC,
	DYNAMIC
};

/*
 * RADIOHYT channel
 */
typedef struct chan_radiohyt_pvt {
	/*
	 * begin of configuration
	 */

	/*!< Channel Name */
	char name[11];
	/*!< Requested extension */
	char exten[AST_MAX_EXTENSION];
	/*!< Context where to start */
	char context[AST_MAX_CONTEXT];
	/*!< Channel Policy */
	int policy;

	/*! Local Address */
	struct ast_sockaddr addr;
	/*! Server Address */
	struct ast_sockaddr srv_addr;
	/*! Stringify Server Address */
	char server_ip[16];

	/*! Timeout in milliseconds to re-connect */
	int connect_timeout;
	/*! Timeout in milliseconds to wait for answer on request */
	int request_timeout;

	/*! Timeout in milliseconds for RTP */
	int rtp_timeout;

	/*! Hyterus API server's client login */
	char login[33];
	/*! Our salt for authenticate on server */
	char salt[17];
	/*! MD5 sum of secret word */
	char md5secret[256];
	/*! Call Session Identifier (UUID) */
	char sid[AST_UUID_STR_LEN];

	/*!< Supported codecs */
	struct ast_format_cap *caps;

	/*! Enable/disable dump all messages sending/receiving throughput channel */
	int debug;

	/*! Timeout values for call states */
	int state_timeout[CHAN_RADIOHYT_ST_COUNT];

	/*
	 * end of configuration
	 */

	/*! Local RTP port number */
	unsigned short rtp_port;
	/*! Server RTP port number */
	unsigned short srv_rtp_port;
	/*! Transport to transmit signalling (JSON_RPC 2.0 over TCP) */
	Transport tport;
	/*! Transport to transmit voice over (RTP over UDP) */
	Transport rtp_tport;
	/*! RTP transport */
	struct ast_rtp_instance *rtpi;

	/*! True, if subscribers are loaded from server */
	int is_subscribers_loaded;

	/*!< Peer's Hyterus RadioId */
	int peer_radio_id;
	/*!< Peer's Hyterus ChannelId */
	int peer_channel_id;

	/*! Direction of active call */
	enum call_direction direction;

	/*! Channel FSM State */
	channel_state state;
	/*! States map */
	RadiohytChannelState states[CHAN_RADIOHYT_ST_COUNT];

	/*! Hyterus API Protocol Message Encoder/Decoder */
	Msg_parser parser;

	/*! Last sent transaction identifier */
	int last_transaction_id;

	/*! Sheduler item ID for re-connect task */
	int sched_conn_id;

	/*! Owner ast_channel */
	struct ast_channel *owner;
	/*! Peer ast_channel */
	struct ast_channel *peer;

	/*! Active session */
	RadiohytSession *session;

	/* Attention: these functions will be called from monitor thread! */
	
	/*! callback is called when connection with server changes state */
	Change_connection_state_callback on_changed_connection_state_cb;
	/*! callback takes pointer to JSON if rc equals to 0 */
	Receive_stations_callback on_receive_station_list_cb;
	/*! callback takes pointer to JSON if rc equals to 0 */
	Receive_groups_callback on_receive_group_list_cb;
	/*! callback takes pointer to JSON if rc equals to 0 */
	Receive_subscribers_callback on_receive_subscriber_list_cb;
	/*! callback is called when incoming call comes in */
	Incoming_call_callback on_incoming_call_cb;
	/*! callback is called when state of call is changed */
	Change_call_state_callback on_changed_call_state_cb;
	/*! callback is called when state of channel is changed */
	Change_channel_state_callback on_changed_channel_state_cb;
	/*! callback is called when state of subscriber is changed */
	Change_subscriber_state_callback on_changed_subscriber_state_cb;

	/*! List of active requests */
	AST_LIST_HEAD_NOLOCK(, radiohyt_request) request_queue;

#ifdef TEST_FRAMEWORK
	struct ast_json *last_send_msg;
#endif

	ENV_THREAD_CHECKER(monitor_thread);

}	Channel;

/* RTP Header */
struct common_rtp_hdr_t {
	unsigned char cc : 4;	/* CSRC count             */
	unsigned char x : 1;	/* header extension flag  */
	unsigned char p : 1;	/* padding flag           */
	unsigned char ver : 2;	/* protocol version       */
	unsigned char pt : 7;	/* payload type           */
	unsigned char m : 1;	/* marker bit             */
	uint16_t seq;		/* sequence number        */
	uint32_t ts;		/* timestamp              */
	uint32_t ssrc;		/* synchronization source */
};

/* RTP Header Extension */
struct common_rtp_hdr_ex_t {
	uint16_t defined_by_profile;
	uint16_t extension_len;
};

struct hyterus_rtp_hdr_ex_t {
	uint16_t profile_id;		// Hyterus Profile ID, must be 0x0015
	uint16_t length;		// Ext header length, must be 3
	uint32_t source_id;		// Slot[6] | L[1] | Source RadioId[24]
	uint32_t receiver_id;		// Receiver RadioId[24] | CallType[8]
	uint32_t reserved;		// Reserved for future using
};

/*
 * RADIOHYT server
 */
/*
typedef struct serv_radiohyt {
	///! Text Address
	char addr[22];
	///! Sock Address
	struct ast_sockaddr sock_addr;

}	Server;
*/
typedef struct ast_sockaddr Server;

/*
 * Interface functions
 */

// Start proxy, initialize session's list
void radiohyt_session_storage_start(void);

// Stop proxy, clear session's list
void radiohyt_session_storage_stop(void);

// Open channel - connect to Hyterus DMR Server
int radiohyt_chan_open(Channel *chan);

// Close channel - disconnect to Hyterus DMR Server
void radiohyt_chan_close(Channel *chan);

// Make outgoing voice call
// P.S. Use timeout_ms only for UnitTests to create delay after function call
int radiohyt_make_call(Channel *chan, int chan_id, int slot_id, int dest_id, int call_type, int timeout_ms);

// Drop active call or reject outgoing call
// P.S. Use timeout_ms only for UnitTests to create delay after function call
int radiohyt_drop_call(Channel *chan, int chan_id, int slot_id, int dest_id, int call_type, int timeout_ms);

// Load station's list
// Result will be returned via on_receive_channel_list_cb callback
int radiohyt_get_stations(Channel *chan, int option);

// Load callgroup's list
// Result will be returned via on_receive_group_list_cb callback
int radiohyt_get_groups(Channel *chan, int option);

// Load subscriber's list
// Result will be returned via on_receive_subscriber_list_cb callback
int radiohyt_get_subscribers(Channel *chan, int option);

// Check status of radio subscriber
int radiohyt_check_radio(Channel *chan, int chan_id, int radio_id, int timeout_ms);

#ifdef TEST_FRAMEWORK
void radiohyt_fake_recv_event(Channel *chan, struct ast_json *msg, int timeout_ms);
void radiohyt_fake_on_break_event(Channel *chan);
void radiohyt_fake_on_connect_event(Channel *chan);
#endif

// Make a copy of session if it is exists or return E_NOT_FOUND
int radiohyt_session_get(Channel *chan, int channel_id, RadiohytSession *session);

#endif

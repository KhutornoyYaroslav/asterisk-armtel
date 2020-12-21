/*
 * aics.h
 *
 * ARMTEL Intelligent Communication Server common header file
 *
 *  Created on: 2015.09.16
 *      Author: v.ivanov
 */

#ifndef _AICS_H_
#define _AICS_H_

#ifndef AICS_VER
/* AICS version format is XXYYZZ,
 * where XX - major version, YY - minor version, ZZ - build number */
#define AICS_VER 144L /* similar to 00.01.32 */
#endif /* AICS_VER */

#define H_PRIORITY "Priority"
#define H_ARMTEL_DIRECTION "Armtel-Direction"
#define H_ARMTEL_SCENARIO "Armtel-Scenario"
#define H_ARMTEL_PLAYLIST "Armtel-Playlist"
#define H_ARMTEL_RELAY "Armtel-Relay"
#define H_ARMTEL_EVENT "Armtel-Event"
#define H_ARMTEL_SIGN "Armtel-Sign"

#define DEFAULT_CALLPRIORITY "normal"  /* AICS support for sip.conf callpriority field */
#define DEFAULT_PRIORITYHANDLE "none"  /* AICS support for sip.conf priorityhandle field */
#define DEFAULT_HANGUPANOUNCE "tt-allbusy"  /* AICS support for sip.conf hangupanounce field */
#define DEFAULT_IPN20UA "IPN2.0"  /* AICS support for sip.conf IPN2.0 devices */
#define AICS_SAFE_STR(x) (x) ? x : "NULL"

#include "asterisk/linkedlists.h"

enum aics_direction {
    AICS_DIRECTION_DUPLEX,
    AICS_DIRECTION_SEND,
    AICS_DIRECTION_RECV,
    AICS_DIRECTION_MUTE,
};

enum aics_scenario {
    AICS_SCENARIO_NONE,
    AICS_SCENARIO_GROUP,
    AICS_SCENARIO_CONFERENCE,
    AICS_SCENARIO_SELECTOR,
    AICS_SCENARIO_CIRCULAR,
};

enum aics_priority_handle {
    AICS_PRIORITY_HANDLE_NONE,
    AICS_PRIORITY_HANDLE_HANGUP,
    AICS_PRIORITY_HANDLE_HOLD,
    AICS_PRIORITY_HANDLE_HANGUP_NOTIFY,
    AICS_PRIORITY_HANDLE_QUEUE,
};

enum aics_cmd_armtel {
    AICS_CMD_NONE,
    AICS_ARMTEL_ASK_WORD,
    AICS_ARMTEL_REMOVE_WORD,
    AICS_ARMTEL_INTERRUPT_WORD,
    AICS_ARMTEL_DIRECTION,
    AICS_ARMTEL_DISKRET3,
    AICS_ARMTEL_CHANGE_STATE_CONF,
};

enum aics_context_armtel {
    AICS_CONTEXT_ARMTEL_DUPLEX,
    AICS_CONTEXT_ARMTEL_SIMPLEX,
    AICS_CONTEXT_ARMTEL_TONE_DUPLEX,
    AICS_CONTEXT_ARMTEL_SYRCULAR,
    AICS_CONTEXT_ARMTEL_SELECTOR,
    AICS_CONTEXT_ARMTEL_CONF,
    AICS_CONTEXT_ARMTEL_LISTEN,
    AICS_CONTEXT_ARMTEL_TALK_BACK,
    AICS_CONTEXT_ARMTEL_TRANSFER,
    AICS_CONTEXT_ARMTEL_ACT_LISTEN=0x10,
    AICS_CONTEXT_ARMTEL_ACT_SPEAK,
    AICS_CONTEXT_ARMTEL_ACT_DUPLEX,
    AICS_CONTEXT_ARMTEL_ACT_LISTEN_BTN_OFF,
    AICS_CONTEXT_ARMTEL_ACT_BUSY,
    AICS_CONTEXT_ARMTEL_ACT_SIMPLEX,
    AICS_CONTEXT_ARMTEL_ACT_DUPLEX_BTN_ON,

};

enum aics_state_armtel_member {
    AICS_STATE_IDLE,
	AICS_STATE_ASK_WORD,
	AICS_STATE_ACTIV,
};


#define AST_CONTROL_ARMTEL_SIG 100
#define AST_CONTROL_ARMTEL_INTERCOM_SIG 101

struct aics_relay {
    unsigned int status: 1;  // 0 .. 1
    unsigned int tm_on:  15; // 0..32767
    unsigned int tm_off: 15; // 0..32767
};

enum aics_event_status {
	AICS_EVENT_OFF,
	AICS_EVENT_ON,
	AICS_EVENT_DROP,
};

struct aics_event {
	unsigned int status: 2;  // 0 .. 2	it is aics_event_status
};

//union aics_event {
//	uint_8t events;
//	struct {
//		unsigned int event1: 1;  // 0 .. 1
//		unsigned int event2: 1;  // 0 .. 1
//		unsigned int event3: 1;  // 0 .. 1
//		unsigned int event4: 1;  // 0 .. 1
//		unsigned int event5: 1;  // 0 .. 1
//		unsigned int event6: 1;  // 0 .. 1
//		unsigned int event7: 1;  // 0 .. 1
//		unsigned int event8: 1;  // 0 .. 1
//	} bits;
//};

struct aics_playlist_item {
    char function;
    char filename[80];
    int repeat;
    int wait;
    AST_LIST_ENTRY(aics_playlist_item) next;
};

struct aics_playlist {
	AST_LIST_HEAD_NOLOCK(, aics_playlist_item) playlist;
};

union aics_proxy_mask {
	unsigned int as_int;
	struct {
		unsigned int priority: 1;
		unsigned int direction: 1;
		unsigned int scenario: 1;
		unsigned int relay: 1;
		unsigned int event: 1;
		unsigned int playlist: 1;
	} as_bits;
};

struct aics_sig_armtel {
    char number[80];
    unsigned char cmd;
    unsigned char prio;
    unsigned char context;
    unsigned char param;
    unsigned int  send:1;

};

struct aics_dial_item {
    void *dial;
    AST_LIST_ENTRY(aics_dial_item) next;
};
struct aics_dial {
	AST_LIST_HEAD_NOLOCK(, aics_dial_item) armtel_dial ;
};

struct aics_proxy_params {
	// params validity
	union aics_proxy_mask validity;
    // call priority: 0 .. 256
    int priority;
    // media direction: duplex, send, recv, mute
    enum aics_direction direction;
    // ICS scenario: simple, group, conference, selector, circular, ...
    enum aics_scenario scenario;
    // Relay control
    struct aics_relay relay[8];
    // Event control
    struct aics_event event[8];
    // Playlist info
    struct aics_playlist playlist;

    struct aics_sig_armtel sig_armtel;
    int prio_member;
    struct aics_dial armtel_dial;
    unsigned int is_member:1;
    unsigned int state_member:2;
//    void *armtel_dial;
};

struct aics_local_params {
    // priority handling rule: none, hangup, hold, hangup-notify, queue
    enum aics_priority_handle priority_handle;
    // call registration
    unsigned int spy:1;
    // support for UPDATE method
    uint32_t pendingupdate;
    // hangup anounce filename
    char *hangup_anounce;

};


//struct aics_pvt_addons {
//	// media direction: sendrecv, sendonly, recvonly, inactive
//	enum aics_direction direction;//char *direction;
//	// last media direction: sendrecv, sendonly, recvonly, inactive
//	enum aics_direction last_direction;
//	// priority handling rule: none, hangup, hold, hangup-notify, queue
//	enum aics_priority_handle priority_handle;
//	// call priority: -3 .. 256
//	int callpriority;//char *callpriority;
//	// hangup anounce filename
//	char *hangup_anounce;
//};

//struct aics_channel_addons {
//	// platform name (user agent)
//	char *platform;
//	// connected channel
//	char *ctrlchan;
//	// ICS scenario: conference, selector, circular, ...
//	enum aics_scenario scenario;//char *scenario;
//	// direction & priority
//	struct aics_pvt_addons pvt_addons;
//};


char *aics_get_version(void);

void aics_send_armtel_sign(struct ast_channel* ast,unsigned char cmd,unsigned char prio,unsigned char context);

void aics_priority_set(struct aics_proxy_params *params, const char *text);
const char *aics_priority_get(struct aics_proxy_params *params);
void aics_direction_set(struct aics_proxy_params *params, const char *text);
const char *aics_direction_get(struct aics_proxy_params *params);
void aics_scenario_set(struct aics_proxy_params *params, const char *text);
const char *aics_scenario_get(struct aics_proxy_params *params);
void aics_relay_set(struct aics_proxy_params *params, const char *text);
const char *aics_relay_get(struct aics_proxy_params *params);
void aics_event_set(struct aics_proxy_params *params, const char *text);
const char *aics_event_get(struct aics_proxy_params *params);
void aics_playlist_set(struct aics_proxy_params *params, const char *text);
const char *aics_playlist_get(struct aics_proxy_params *params);

enum aics_priority_handle aics_priority_handle_from_str(const char *text);
const char *aics_priority_handle_to_str(const int code);
enum aics_event_status aics_event_status_from_str(const char *text);
const char *aics_event_status_to_str(const int code);

int aics_priority_from_str(const char *text);
const char *aics_priority_to_str(const int code);
enum aics_direction aics_direction_from_str(const char *text);
const char *aics_direction_to_str(const int code);
enum aics_scenario aics_scenario_from_str(const char *text);
const char *aics_scenario_to_str(const int code);
int aics_playlist_from_str(struct aics_playlist *head, const char *text);
const char *aics_playlist_to_str(struct aics_playlist *head);
int aics_relay_from_str(struct aics_relay *array, const int size, const char *text);
const char *aics_relay_to_str(struct aics_relay *array, const int size);
int aics_event_from_str(struct aics_event *array, const int size, const char *text);
const char *aics_event_to_str(struct aics_event *array, const int size);
//void aics_debug_channels(void);

void aics_proxy_params_init(struct aics_proxy_params *params);
void aics_proxy_params_copy(struct aics_proxy_params *dst, struct aics_proxy_params *src);
void aics_proxy_params_free(struct aics_proxy_params *params);

void aics_local_params_init(struct aics_local_params *params);
void aics_local_params_copy(struct aics_local_params *dst, struct aics_local_params *src);
void aics_local_params_free(struct aics_local_params *params);

void aics_num_from_name(char*num,const char* name);
const char* aics_get_name_cmd(unsigned char id);

//void aics_pvt_addons_init(struct aics_pvt_addons *init);
//void aics_pvt_addons_copy(struct aics_pvt_addons *dst, struct aics_pvt_addons *src);
//void aics_pvt_addons_free(struct aics_pvt_addons *done);
//void aics_pvt_addons_notice(struct aics_pvt_addons *info, char *prefix);
//void aics_pvt_addons_set_direction(struct aics_pvt_addons *info, enum aics_direction direction);
//
//void aics_addons_init(struct aics_channel_addons *init);
//void aics_addons_copy(struct aics_channel_addons *dst, struct aics_channel_addons *src);
//void aics_addons_free(struct aics_channel_addons *done);
//void aics_addons_notice(struct aics_channel_addons *info, char *prefix);

#endif /* _AICS_H_ */

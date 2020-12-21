/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 * Andreas 'MacBrody' Brodmann <andreas.brodmann@gmail.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \author Joshua Colp <jcolp@digium.com>
 * \author Andreas 'MacBrody' Broadmann <andreas.brodmann@gmail.com>
 *
 * \brief Multicast RTP Paging Channel
 *
 * \ingroup channel_drivers
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"



ASTERISK_FILE_VERSION(__FILE__, "$Revision: 410157 $")

#include <fcntl.h>
#include <sys/signal.h>

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/acl.h"
#include "asterisk/callerid.h"
#include "asterisk/file.h"
#include "asterisk/cli.h"
#include "asterisk/app.h"
#include "asterisk/rtp_engine.h"
#include "asterisk/causes.h"
#include "asterisk/tcptls.h"
#include "asterisk/linkedlists.h"
#include "asterisk/netsock2.h"

static const char tdesc[] = "Modbus Driver";

#define DEFAULT_AUTHLIMIT            100
#define DEFAULT_AUTHTIMEOUT          30
#define STANDARD_MODBUS_PORT	5025
#define MODBUS_MIN_PACKET            2048
#define MODBUS_MAX_PACKET_SIZE          4096
static const int HASH_MODBUS_SIZE = 563;
#define MODBUS_FILE_CONFIG "modbus.conf"
#define MODBUS_MAX_REGISER          16
#define MODBUS_VERSION          "1.8"

enum modbus_func {
READ_COIL_STATUS = 0x01,	/*Чтение DO	Read Coil Status	Дискретное	Чтение*/
READ_INPUT_STATUS = 0x02,	/* Чтение DI	Read Input Status	Дискретное	Чтение*/
READ_HOLDING_REGISTERS =0x03,	/*Чтение AO	Read Holding Registers	16 битное	Чтение*/
READ_INPUT_REGISTERS =0x04,	/*Чтение AI		16 битное	Чтение*/
FORSE_SINGLE_COIL =0x05,	/*Запись одного DO	Force Single Coil	Дискретное	Запись*/
PRESET_SINGLE_REGISTER =0x06,	/*Запись одного AO		16 битное	Запись*/
FORSE_MULTIPLE_COILS =0x0F,	/*Запись нескольких DO		Дискретное	Запись*/
PRESET_MULTIPLE_REGISTERS=0x10,	/*Запись нескольких AO		16 битное	Запись*/
};
enum modbus_error {
 ILLEGAL_FUNCTION =   1,
 ILLEGAL_DATA_ADDRESS =   2,
 ILLEGAL_DATA_VALUE   = 3,
 FAILURE_IN_ASSOCIATED  =  4,
 ACKNOWLEDGE   = 5,
 BUSY_REJECTED  =  6,
 NAK_NEGATIVE  =  7,
};



enum modbus_tcptls_alert {
	TCPTLS_ALERT_DATA,  /*!< \brief There is new data to be sent out */
	TCPTLS_ALERT_STOP,  /*!< \brief A request to stop the tcp_handler thread */
};

struct modbus_idxbit {
	struct ast_str* num; /* number playpage */
    uint8_t  bit;
};


static struct modbus_reg{
   struct ao2_container *idx_num; /* */
   uint16_t  reg_num;
   uint16_t  reg_dat;
};

struct tcptls_packet {
	AST_LIST_ENTRY(tcptls_packet) entry;
	struct ast_str *data;
	size_t len;
};

struct modbus_threadinfo {
	/*! TRUE if the thread needs to kill itself.  (The module is being unloaded.) */
	int stop;
	int alert_pipe[2];          /*! Used to alert tcptls thread when packet is ready to be written */
	pthread_t threadid;
	struct ast_tcptls_session_instance *tcptls_session;
	enum ast_transport type;    /*!< We keep a copy of the type here so we can display it in the connection list */
	AST_LIST_HEAD_NOLOCK(, tcptls_packet) packet_q;
};

struct modbus_socket {
	enum ast_transport type;  /*!< UDP, TCP or TLS */
	int fd;                   /*!< Filed descriptor, the actual socket */
	uint16_t port;
	struct ast_tcptls_session_instance *tcptls_session;  /* If tcp or tls, a socket manager */
	struct ast_websocket *ws_session; /*! If ws or wss, a WebSocket session */
};

struct modbus_request {
	char authenticated;     /*!< non-zero if this request was authenticated */
	/* XXX Do we need to unref socket.ser when the request goes away? */
	struct modbus_socket socket;          /*!< The socket used foLoad configr this request */
	AST_LIST_ENTRY(modbus_request) next;
};



/* Forward declarations */
static struct ast_channel *modbus_request(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *data, int *cause);
static int modbus_call(struct ast_channel *ast, const char *dest, int timeout);
static int modbus_hangup(struct ast_channel *ast);
static struct ast_frame *modbus_read(struct ast_channel *ast);
static int modbus_write(struct ast_channel *ast, struct ast_frame *f);


static void *modbus_tcp_worker_fn(void *);
static void *_modbus_tcp_helper_thread(struct ast_tcptls_session_instance *);
static struct modbus_threadinfo *modbus_threadinfo_create(struct ast_tcptls_session_instance *, int );
static void modbus_threadinfo_destructor(void *);
static int modbus_tcptls_read(struct modbus_request* , struct ast_tcptls_session_instance *,int , time_t );
static void modbus_show(struct ast_cli_args *a);
static char *handle_modbus_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static char *handle_modbus_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static char *handle_modbus_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static char *handle_modbus_help(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static void modbus_reload();
static void modbus_parse(struct ast_tcptls_session_instance *tcptls_session,char*,int len);
static int modbus_check_register(uint16_t reg,uint16_t reg_data,char*);
static struct modbus_reg * find_reg(struct ao2_container * cont, int idx);
struct modbus_idxbit * find_bit(struct ao2_container * cont, int idx);
static int modbus_init_page(char* num,char*ip,int,int);
static int modbus_stop_page(char* num,char*ip);
static struct modbus_pvt  *find_pvt_page(const char *page);
static struct modbus_pvt  *find_pvt_ip(const char *ip);
static void pvt_destructor(void *obj);
static struct modbus_pvt  *init_pvt(const char *,const char *,int,int);

void str_to_hex(char* hex ,char* str,unsigned char len);


struct ast_sockaddr bindaddr;
static int unauth_sessions = 0;
static int authlimit = DEFAULT_AUTHLIMIT;
static int authtimeout = DEFAULT_AUTHTIMEOUT;
static struct ao2_container *threadt; /*! \brief  The table of TCP threads */
static uint16_t modbus_port;
static struct ao2_container *mregisters;
static int modbus_debug=0;
static struct ao2_container *pvts; /*! \brief  The table of TCP threads */
static uint32_t modbus_chan;

static struct modbus_pvt {
	AST_DECLARE_STRING_FIELDS(
		/*! Name of the device */
		AST_STRING_FIELD(name);
		/*! Default context for outgoing calls */
		AST_STRING_FIELD(context);
		/*! Default extension for outgoing calls */
		AST_STRING_FIELD(exten);
		/*! Default CallerID number */
		AST_STRING_FIELD(cid_num);
		/*! Default CallerID name */
		AST_STRING_FIELD(cid_name);

		AST_STRING_FIELD(page);
		AST_STRING_FIELD(ip);
	);
	int chan;
	int  reg;
	int  bit;
	/*! Current channel for this device */
	struct ast_channel *owner;
	/*! ID for the stream monitor thread */
	pthread_t thread;
};



/*! \brief The TCP server definition */
static struct ast_tcptls_session_args modbus_tcp_desc = {
	.accept_fd = -1,
	.master = AST_PTHREADT_NULL,
	.tls_cfg = NULL,
	.poll_timeout = -1,
	.name = "MODBUS TCP server",
	.accept_fn = ast_tcptls_server_root,
	.worker_fn = modbus_tcp_worker_fn,
};
/* Channel driver declaration */
static struct ast_channel_tech modbus_tech = {
	.type = "Modbus",
	.description = "Modbus Channel Driver",
	.requester = modbus_request,
	.call = modbus_call,
	.hangup = modbus_hangup,
	.read = modbus_read,
	.write = modbus_write,
};

static struct ast_cli_entry modbus_cli[] = {
	AST_CLI_DEFINE(handle_modbus_show, "Displays modbus data"),
	AST_CLI_DEFINE(handle_modbus_reload, "Reload modbus configuration"),
	AST_CLI_DEFINE(handle_modbus_debug, "Enables modbus debug {on|off}"),
	AST_CLI_DEFINE(handle_modbus_help, "Displays modbus information"),

};

static char *handle_modbus_help(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "modbus help";
		e->usage =
				"Usage: modbus help\n"
			    "Displays modbus information\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
   ast_verbose("\nOnly two Modbus commands supported\n--Read Holding Registers(0x3)\n--Preset Single Register(0x6)\n");
	return CLI_SUCCESS;


}
static char *handle_modbus_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "modbus show";
		e->usage =
				"Usage: modbus show\n"
			    "Displays modbus data\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	modbus_show(a);
	return CLI_SUCCESS;
}
static char *handle_modbus_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
		switch (cmd) {
		case CLI_INIT:
			e->command = "modbus reload";
			e->usage =
					"Usage: modbus reload\n"
				    "Reload modbus configuration\n";
			return NULL;
		case CLI_GENERATE:
			return NULL;
		}
		modbus_reload();
		return CLI_SUCCESS;
}
static char *handle_modbus_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
		switch (cmd) {
		case CLI_INIT:
			e->command = "modbus debug {on|off}";
			e->usage =
					"Usage: modbus debug on|off\n"
				    " Enables debugging modbus\n";
			return NULL;
		case CLI_GENERATE:
			return NULL;
		}

		if (!strcasecmp(a->argv[2], "on")) {
			modbus_debug = 1;
		} else if (!strcasecmp(a->argv[2], "off")) {
			modbus_debug = 0;

		}
		return CLI_SUCCESS;
}



void str_to_hex(char* hex ,char* str,unsigned char len )
{
	int i,k=0,j;
	if (len) j=len;
	else j=strlen(str);
	if(j>32) j=32;

	for(i=0;i<j;i++) k+=sprintf(hex+k,"%3x",(unsigned char)*str++);
    *(hex+k)=0;
}


static int modbus_init_page(char* num,char*ip,int reg,int bit)
{
struct ast_channel *chan;
char ch[16];
struct modbus_pvt* mp= init_pvt(num,ip,reg,bit);
 if(mp){
    snprintf(ch, sizeof(ch),"%d",mp->chan);
    chan = ast_channel_alloc(0, AST_STATE_RINGING, mp->cid_num, mp->cid_name, NULL, num, "ArmtelICS", NULL, NULL, 0, "Modbus/%s",ch);
	if (!chan) {
		        ast_log(LOG_ERROR, "ast_channel_alloc error\n");
		return 0;
	}

	ast_channel_tech_set(chan, &modbus_tech);
//	ast_format_set(ast_channel_readformat(chan), AST_FORMAT_SLINEAR16, 0);
//	ast_format_set(ast_channel_writeformat(chan), AST_FORMAT_SLINEAR16, 0);
//	ast_format_cap_add(ast_channel_nativeformats(chan), ast_channel_readformat(chan));
	ast_format_set(ast_channel_readformat(chan),AST_FORMAT_ALAW, 0);
	ast_format_cap_add(ast_channel_nativeformats(chan), ast_channel_readformat(chan));
	ast_channel_tech_pvt_set(chan, mp);
    ast_channel_unlock(chan);
	mp->owner = chan;

	if (ast_pbx_start(chan)) {
			ast_channel_hangupcause_set(chan, AST_CAUSE_SWITCH_CONGESTION);
			ast_hangup(chan);
			chan = NULL;
	        ast_log(LOG_ERROR, "ast_pbx_start error\n");
	 }
 }
	return 0;
}

static int modbus_stop_page(char* num,char* ip)
{
 struct ast_channel *chan;
  struct modbus_pvt* mp=NULL;
  mp= find_pvt_page(num);
    	ast_log(LOG_WARNING, "find PVTS=%d'\n",ao2_container_count(pvts));
  if(mp != NULL){
       	    ast_verbose("----pvt=[name=%s,page=%s,ip=%s]\n",mp->name,mp->page,mp->ip);
    chan=mp->owner;
    if(chan){
      ast_softhangup_nolock(chan,AST_SOFTHANGUP_DEV);
//	ast_channel_hangupcause_set(chan,AST_CAUSE_NORMAL_CLEARING);
//	ast_hangup(chan);
	 chan = NULL;
    }
    ao2_ref(mp, -1);
//   	ao2_unlock(mp);
//	pvt_destructor(mp);

 }
}
/*! \brief creates a sip_threadinfo object and links it into the threadt table. */
static struct modbus_threadinfo *modbus_threadinfo_create(struct ast_tcptls_session_instance *tcptls_session, int transport)
{
	struct modbus_threadinfo *th;

	if (!tcptls_session || !(th = ao2_alloc(sizeof(*th), modbus_threadinfo_destructor))) {
		return NULL;
	}

	th->alert_pipe[0] = th->alert_pipe[1] = -1;

	if (pipe(th->alert_pipe) == -1) {
		ao2_t_ref(th, -1, "Failed to open alert pipe on sip_threadinfo");
		ast_log(LOG_ERROR, "Could not create sip alert pipe in tcptls thread, error %s\n", strerror(errno));
		return NULL;
	}
	ao2_t_ref(tcptls_session, +1, "tcptls_session ref for sip_threadinfo object");
	th->tcptls_session = tcptls_session;
	th->type = transport ? transport : (tcptls_session->ssl ? AST_TRANSPORT_TLS: AST_TRANSPORT_TCP);
	ao2_t_link(threadt, th, "Adding new tcptls helper thread");
	ao2_t_ref(th, -1, "Decrementing threadinfo ref from alloc, only table ref remains");
	return th;
}

static void modbus_threadinfo_destructor(void *obj)
{
	struct modbus_threadinfo *th = obj;
	struct tcptls_packet *packet;

	if (th->alert_pipe[1] > -1) {
		close(th->alert_pipe[0]);
	}
	if (th->alert_pipe[1] > -1) {
		close(th->alert_pipe[1]);
	}
	th->alert_pipe[0] = th->alert_pipe[1] = -1;

	while ((packet = AST_LIST_REMOVE_HEAD(&th->packet_q, entry))) {
		ao2_t_ref(packet, -1, "thread destruction, removing packet from frame queue");
	}

	if (th->tcptls_session) {
		ao2_t_ref(th->tcptls_session, -1, "remove tcptls_session for sip_threadinfo object");
	}
}

static void *modbus_tcp_worker_fn(void *data)
{
	struct ast_tcptls_session_instance *tcptls_session = data;

	return _modbus_tcp_helper_thread(tcptls_session);
}


static int modbus_check_register(uint16_t reg,uint16_t reg_data,char*ip)
{
uint8_t i;
struct modbus_reg *r=NULL;
struct modbus_idxbit *b=NULL;
uint16_t t;

ao2_lock(mregisters);
  r= find_reg(mregisters,reg);
  if(r !=NULL){
     t=r->reg_dat;
     for ( i =0;i<16;i++){ // проверяем каждый бит
		if((t & (1<<i)) !=(reg_data & (1<<i))){
		    b=find_bit(r->idx_num,i);
		    if(b !=NULL){
			  if(reg_data  & (1<<i)){
			   	ast_log(LOG_NOTICE, "Modbus call number = %s[reg=%d;bit=%d]\n",b->num->str,reg,i);
			   	modbus_init_page(b->num->str,ip,reg,i);
			  }
			  else{
			   	ast_log(LOG_NOTICE, "Modbus hangup number = %s'\n",b->num->str);
			   	modbus_stop_page(b->num->str,ip);
			  }
			}
		}
	}
    r->reg_dat=reg_data;
    ao2_ref(r, -1);
    ao2_unlock(mregisters);
    return 0;
  }
  ao2_unlock(mregisters);
  return ILLEGAL_DATA_ADDRESS;
}

static void modbus_parse(struct ast_tcptls_session_instance *tcptls_session,char* buf,int len)
{
	char sbuf[128];
	uint16_t reg;
	uint16_t reg_data;
	int i,j,res,cnt;
    char hex[1028];
   struct modbus_reg *r=NULL;

    memcpy(sbuf,buf,len);
    if(modbus_debug){
   	        str_to_hex(hex,sbuf,len);
	        ast_verbose("Modbus(%s)> [%s]\n",ast_sockaddr_stringify(&tcptls_session->remote_address), hex);
    }
    switch(buf[7]){
     case READ_HOLDING_REGISTERS:
          reg =((buf[8]<<8) & 0xFF00) | (buf[9] & 0xFF);
          cnt =((buf[10]<<8) & 0xFF00) | (buf[11] & 0xFF);
          j=9;
          for(i=0;i<cnt;i++){
            r= find_reg(mregisters,reg);
			if (r != NULL){
				   reg_data= r->reg_dat;
                   sbuf[j++]=((((0xFF00 & reg_data)>>8) & 0xFF));
                   sbuf[j++]=(0xFF & reg_data);
				   reg++;
			}
			else{
 	           sbuf[7]|=0x80;
 	           sbuf[8]=ILLEGAL_DATA_ADDRESS;
 	           sbuf[5]|=3;
 	           len=9;
 	           goto send;
            }
          }
          sbuf[8]=(cnt*2) & 0xFF;
          sbuf[5]=(cnt*2+3) & 0xFF;
          len=cnt*2+9;
     break;
     case PRESET_SINGLE_REGISTER:
          reg =((buf[8]<<8) & 0xFF00) | (buf[9] & 0xFF);
          reg_data =((buf[10]<<8) & 0xFF00) | (buf[11] & 0xFF);
// 	      ast_log(LOG_WARNING, "PRESET_SINGLE_REGISTER = %d[%X]\n", reg,reg_data);
 	      res=modbus_check_register(reg,reg_data,ast_sockaddr_stringify(&tcptls_session->remote_address));
 	      if(res){
 	        sbuf[7]|=0x80;
 	        sbuf[8]=res;
 	        sbuf[5]|=3;
 	        len=9;
 	      }
     break;
     default:
        sbuf[7]|=0x80;
        sbuf[8]=ILLEGAL_FUNCTION;
        sbuf[5]|=3;
        len=9;
     break;
    }
send:
    if (ast_tcptls_server_write(tcptls_session, sbuf, len) == -1) {
			ast_log(LOG_WARNING, "Failure to write to tcp/tls socket\n");
	}
    if(modbus_debug){
   	        str_to_hex(hex,sbuf,len);
	        ast_verbose("Modbus(%s) < [%s]\n",ast_sockaddr_stringify(&tcptls_session->remote_address), hex);
    }

}


static int modbus_tcptls_read(struct modbus_request *req, struct ast_tcptls_session_instance *tcptls_session,
		int authenticated, time_t start)
{
		size_t datalen;
		char readbuf[128];
		int res;
			res = ast_tcptls_server_read(tcptls_session, readbuf, sizeof(readbuf) - 1);
			if (res < 0) {
				if (errno == EAGAIN || errno == EINTR) {
				//	continue;
				}
				ast_debug(2, "MODBUS TCP/TLS server error when receiving data\n");
				return -1;
			} else if (res == 0) {
				ast_debug(2, "MODBUS TCP/TLS server has shut down\n");
				return -1;
			}
			readbuf[res] = '\0';
//			ast_log(LOG_WARNING, "modbus_parse\n");
            modbus_parse(tcptls_session,readbuf,res);
		    datalen = res;
			ast_str_reset(tcptls_session->overflow_buf);

		if (datalen > MODBUS_MAX_PACKET_SIZE) {
			ast_log(LOG_WARNING, "Rejecting TCP/TLS packet from '%s' because way too large: %zu\n",
				ast_sockaddr_stringify(&tcptls_session->remote_address), datalen);
			return -1;
		}

	return 0;
}



/*! \brief SIP TCP thread management function
	This function reads from the socket, parses the packet into a request
*/
static void *_modbus_tcp_helper_thread(struct ast_tcptls_session_instance *tcptls_session)
{
	int res, timeout = -1, authenticated = 0, flags;
	time_t start;
	struct modbus_request req = { 0, } ;//, reqcpy = { 0, };
	struct modbus_threadinfo *me = NULL;
//	char buf[1024] = "";
	struct pollfd fds[2] = { { 0 }, { 0 }, };
	struct ast_tcptls_session_args *ca = NULL;

	if (!tcptls_session->client) {

		if (ast_atomic_fetchadd_int(&unauth_sessions, +1) >= authlimit) {
			/* unauth_sessions is decremented in the cleanup code */
			goto cleanup;
		}

		if ((flags = fcntl(tcptls_session->fd, F_GETFL)) == -1) {
			ast_log(LOG_ERROR, "error setting socket to non blocking mode, fcntl() failed: %s\n", strerror(errno));
			goto cleanup;
		}

		flags |= O_NONBLOCK;
		if (fcntl(tcptls_session->fd, F_SETFL, flags) == -1) {
			ast_log(LOG_ERROR, "error setting socket to non blocking mode, fcntl() failed: %s\n", strerror(errno));
			goto cleanup;
		}

		if (!(me = modbus_threadinfo_create(tcptls_session, tcptls_session->ssl ? AST_TRANSPORT_TLS : AST_TRANSPORT_TCP))) {
			goto cleanup;
		}
		ao2_t_ref(me, +1, "Adding threadinfo ref for tcp_helper_thread");
	} else {
		struct modbus_threadinfo tmp = {
			.tcptls_session = tcptls_session,
		};

		if ((!(ca = tcptls_session->parent)) ||
			(!(me = ao2_t_find(threadt, &tmp, OBJ_POINTER, "ao2_find, getting sip_threadinfo in tcp helper thread"))) ||
			(!(tcptls_session = ast_tcptls_client_start(tcptls_session)))) {
			goto cleanup;
		}
	}

	flags = 1;
	if (setsockopt(tcptls_session->fd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof(flags))) {
		ast_log(LOG_ERROR, "error enabling TCP keep-alives on MODBUS socket: %s\n", strerror(errno));
		goto cleanup;
	}

	me->threadid = pthread_self();
	ast_debug(2, "Starting thread for %s server\n", tcptls_session->ssl ? "TLS" : "TCP");


//	ast_log(LOG_WARNING, "Setting TCP socket address to %s\n",
//				  ast_sockaddr_stringify(&modbus_tcp_desc.local_address));
	/* set up pollfd to watch for reads on both the socket and the alert_pipe */
	fds[0].fd = tcptls_session->fd;
	fds[1].fd = me->alert_pipe[0];
	fds[0].events = fds[1].events = POLLIN | POLLPRI;


	if(time(&start) == -1) {
		ast_log(LOG_ERROR, "error executing time(): %s\n", strerror(errno));
		goto cleanup;
	}

	/*
	 * We cannot let the stream exclusively wait for data to arrive.
	 * We have to wake up the task to send outgoing messages.
	 */
	ast_tcptls_stream_set_exclusive_input(tcptls_session->stream_cookie, 0);

	ast_tcptls_stream_set_timeout_sequence(tcptls_session->stream_cookie, ast_tvnow(),
		tcptls_session->client ? -1 : (authtimeout * 1000));

	ast_log(LOG_NOTICE, "Starting thread for %s server[%s]{%d|%d}\n", tcptls_session->ssl ? "TLS" : "TCP",ast_sockaddr_stringify(&tcptls_session->remote_address),fds[0].fd,fds[1].fd);


	for (;;) {
        timeout = 1000;
//		if (ast_str_strlen(tcptls_session->overflow_buf) == 0) {
			res = ast_poll(fds, 2, timeout); /* polls for both socket and alert_pipe */
			if (res < 0) {
				ast_debug(2, "MODBUS %s server :: ast_wait_for_input returned %d\n", tcptls_session->ssl ? "TLS": "TCP", res);
				goto cleanup;
			} else if (res == 0) {
				/* timeout */
				fds[0].revents = 0;
			}
//		}
		/*
		 * handle the socket event, check for both reads from the socket fd or TCP overflow buffer,
		 * and writes from alert_pipe fd.
		 */
//		if (fds[0].revents || (ast_str_strlen(tcptls_session->overflow_buf) > 0)) { /* there is data on the socket to be read */
		if (fds[0].revents ) { /* there is data on the socket to be read */
			fds[0].revents = 0;

			res = modbus_tcptls_read(&req, tcptls_session, authenticated, start);
			if (res < 0) {
				goto cleanup;
			}
	//        ast_log(LOG_WARNING, "modbus_tcptls_read\n");

		}

		if (fds[1].revents) { /* alert_pipe indicates there is data in the send queue to be sent */
			enum modbus_tcptls_alert alert;
			struct tcptls_packet *packet;

			fds[1].revents = 0;

			if (read(me->alert_pipe[0], &alert, sizeof(alert)) == -1) {
				ast_log(LOG_ERROR, "read() failed: %s\n", strerror(errno));
		//		continue;
			}

			switch (alert) {
			case TCPTLS_ALERT_STOP:
				goto cleanup;
			case TCPTLS_ALERT_DATA:
				ao2_lock(me);
				if (!(packet = AST_LIST_REMOVE_HEAD(&me->packet_q, entry))) {
					ast_log(LOG_WARNING, "TCPTLS thread alert_pipe indicated packet should be sent, but frame_q is empty\n");
				}
				ao2_unlock(me);

				if (packet) {
					if (ast_tcptls_server_write(tcptls_session, ast_str_buffer(packet->data), packet->len) == -1) {
						ast_log(LOG_WARNING, "Failure to write to tcp/tls socket\n");
					}
					ao2_t_ref(packet, -1, "tcptls packet sent, this is no longer needed");
				}
				break;
			default:
				ast_log(LOG_ERROR, "Unknown tcptls thread alert '%u'\n", alert);
			}
		}
	}

	ast_debug(2, "Shutting down thread for %s server\n", tcptls_session->ssl ? "TLS" : "TCP");

cleanup:
	if (tcptls_session && !tcptls_session->client && !authenticated) {
		ast_atomic_fetchadd_int(&unauth_sessions, -1);
	}

	if (me) {
		ao2_t_unlink(threadt, me, "Removing tcptls helper thread, thread is closing");
		ao2_t_ref(me, -1, "Removing tcp_helper_threads threadinfo ref");
	}
	/* if client, we own the parent session arguments and must decrement ref */
	if (ca) {
		ao2_t_ref(ca, -1, "closing tcptls thread, getting rid of client tcptls_session arguments");
	}

	if (tcptls_session) {
		ao2_lock(tcptls_session);
		ast_tcptls_close_session_file(tcptls_session);
		tcptls_session->parent = NULL;
		ao2_unlock(tcptls_session);

		ao2_ref(tcptls_session, -1);
		tcptls_session = NULL;
	}
	return NULL;
}

static int threadt_hash_cb(const void *obj, const int flags)
{
	const struct modbus_threadinfo *th = obj;

	return ast_sockaddr_hash(&th->tcptls_session->remote_address);
}

static int threadt_cmp_cb(void *obj, void *arg, int flags)
{
	struct modbus_threadinfo *th = obj, *th2 = arg;

	return (th->tcptls_session == th2->tcptls_session) ? CMP_MATCH | CMP_STOP : 0;
}


/*! \brief Function called when we should read a frame from the channel */
static struct ast_frame  *modbus_read(struct ast_channel *ast)
{
	return &ast_null_frame;
}

/*! \brief Function called when we should write a frame to the channel */
static int modbus_write(struct ast_channel *ast, struct ast_frame *f)
{
//	struct ast_rtp_instance *instance = ast_channel_tech_pvt(ast);

//	return ast_rtp_instance_write(instance, f);
	return 0;
}

/*! \brief Function called when we should actually call the destination */
static int modbus_call(struct ast_channel *ast, const char *dest, int timeout)
{
	struct ast_rtp_instance *instance = ast_channel_tech_pvt(ast);

	ast_queue_control(ast, AST_CONTROL_ANSWER);

	return ast_rtp_instance_activate(instance);
}

/*! \brief Function called when we should hang the channel up */
static int modbus_hangup(struct ast_channel *ast)
{
struct modbus_pvt *instance = ast_channel_tech_pvt(ast);
struct modbus_reg *r=NULL;

  ao2_lock(pvts);
  if(instance){
   // ast_log(LOG_NOTICE, "Hangup PVTS =%d '\n",ao2_container_count(pvts));
    ast_log(LOG_NOTICE, "Hangup PVT[name=%s;reg=%d;bit=%d]\n",instance->name,instance->reg,instance->bit);
//cliring bit modbus !!!!!!!! :)
    r= find_reg(mregisters,instance->reg);
	r->reg_dat &=~(1<<instance->bit);

    ao2_unlink(pvts, instance);
	pvt_destructor(instance);
   }
  ao2_unlock(pvts);

  ast_channel_tech_pvt_set(ast, NULL);
	return 0;
}

/*! \brief Function called when we should prepare to call the destination */
static struct ast_channel *modbus_request(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *data, int *cause)
{
	char *tmp = ast_strdupa(data), *multicast_type = tmp, *destination, *control;
	struct ast_rtp_instance *instance;
	struct ast_sockaddr control_address;
	struct ast_sockaddr destination_address;
	struct ast_channel *chan;
	struct ast_format fmt;
	ast_best_codec(cap, &fmt);

	ast_sockaddr_setnull(&control_address);

	/* If no type was given we can't do anything */
	if (ast_strlen_zero(multicast_type)) {
		goto failure;
	}

	if (!(destination = strchr(tmp, '/'))) {
		goto failure;
	}
	*destination++ = '\0';

	if ((control = strchr(destination, '/'))) {
		*control++ = '\0';
		if (!ast_sockaddr_parse(&control_address, control,
					PARSE_PORT_REQUIRE)) {
			goto failure;
		}
	}

	if (!ast_sockaddr_parse(&destination_address, destination,
				PARSE_PORT_REQUIRE))
	{
		goto failure;
	}

	if (!(instance = ast_rtp_instance_new("multicast", NULL, &control_address, multicast_type))) {
		goto failure;
	}

	if (!(chan = ast_channel_alloc(1, AST_STATE_DOWN, "", "", "", "", "", assignedids, requestor, 0, "MulticastRTP/%p", instance))) {
		ast_rtp_instance_destroy(instance);
		goto failure;
	}
	ast_rtp_instance_set_channel_id(instance, ast_channel_uniqueid(chan));
	ast_rtp_instance_set_remote_address(instance, &destination_address);

	ast_channel_tech_set(chan, &modbus_tech);

	ast_format_cap_add(ast_channel_nativeformats(chan), &fmt);
	ast_format_copy(ast_channel_writeformat(chan), &fmt);
	ast_format_copy(ast_channel_rawwriteformat(chan), &fmt);
	ast_format_copy(ast_channel_readformat(chan), &fmt);
	ast_format_copy(ast_channel_rawreadformat(chan), &fmt);

	ast_channel_tech_pvt_set(chan, instance);

	ast_channel_unlock(chan);

	return chan;

failure:
	*cause = AST_CAUSE_FAILURE;
	return NULL;
}

static void pvt_destructor(void *obj)
{
	struct modbus_pvt *pvt = obj;
	modbus_chan &=~(1<<pvt->chan);
	ast_string_field_free_memory(pvt);
}
static int pvt_hash_cb(const void *obj, const int flags)
{
	const struct modbus_pvt *pvt = obj;

	return ast_str_case_hash(pvt->name);
}

static int pvt_cmp_cb(void *obj, void *arg, int flags)
{
	struct modbus_pvt *pvt = obj, *pvt2 = arg;

	return !strcasecmp(pvt->name, pvt2->name) ? CMP_MATCH | CMP_STOP : 0;
}
static int pvt_cmp_page(void *obj, void *arg, int flags)
{
	struct modbus_pvt *pvt = obj;
	char* page = (char*)arg;

	return !strcasecmp(pvt->page, page) ? CMP_MATCH | CMP_STOP : 0;
}
static int pvt_cmp_ip(void *obj, void *arg, int flags)
{
	struct modbus_pvt *pvt = obj;
	char* ip = (char*)arg;

	return !strcasecmp(pvt->ip, ip) ? CMP_MATCH | CMP_STOP : 0;
}
static struct modbus_pvt  *find_pvt_page(const char *page)
{
  struct modbus_pvt *f=NULL;
	   f=ao2_callback(pvts,0,pvt_cmp_page,(void*)page);
	  return(f);
}
static struct modbus_pvt  *find_pvt_ip(const char *ip)
{
  struct modbus_pvt *f=NULL;
	   f=ao2_callback(pvts,0,pvt_cmp_ip,(void*)ip);
	  return(f);
}
static struct modbus_pvt  *init_pvt(const char *p,const char *ii,int reg,int bit)
{
  struct modbus_pvt *f=NULL;
  int i;

   f= find_pvt_page(p);
   if(f == NULL){
	 f=ao2_alloc(sizeof(*f),pvt_destructor);
	 if(f){
	  f->reg=reg;
	  f->bit=bit;
      for (i =0;i<32;i++){ // проверяем каждый бит
		if((modbus_chan & (1<<i))==0){
		   modbus_chan |=(1<<i);
		   f->chan=i;
		   break;
		}
      }
      if (ast_string_field_init(f, 32))
		return NULL;
      ao2_lock(pvts);
//      ast_string_field_set(f, name, "Modbus");
//      ast_string_field_set(f, exten, "666");
      ast_string_field_set(f, cid_num, p);
      ast_string_field_set(f, page, p);
      ast_string_field_set(f, ip, ii);
      ao2_link(pvts, f);
      ao2_unlock(pvts);
      return f;
     }
   }
   else{
     ao2_ref(f, -1);
   }
   return NULL;
}



static int modbus_bits_hash(const void *obj,const int flag)
{
	const struct modbus_idxbit * bit =obj;
	return (bit->bit);

}
static int modbus_bits_cmp( void *obj,void* arg, int flag)
{
	struct modbus_idxbit * bit =obj;
	struct modbus_idxbit * bit1 =arg;
    return(bit->bit == bit1->bit ? CMP_MATCH | CMP_STOP :0);
}

static void  modbus_bits_destructor(void* obj)
{
	struct modbus_idxbit *bits =obj;

   	   ast_free(bits->num);
}
int bit_cmp(void *obj,void* arg, int flag)
{
	struct modbus_idxbit *b =obj;
    return(b->bit == (int)arg ? CMP_MATCH | CMP_STOP :0);
}

struct modbus_idxbit * find_bit(struct ao2_container * cont, int idx)
{
  struct modbus_idxbit *f=NULL;
	   f=ao2_callback(cont,0,bit_cmp,(void*)idx);
	  return(f);
}
static int modbus_reg_hash(const void *obj,const int flag)
{
	const struct modbus_reg * r =obj;
	return (r->reg_num);

}
static int modbus_reg_cmp( void *obj,void* arg, int flag)
{
	struct modbus_reg * r =obj;
	struct modbus_reg * r1 =arg;
    return(r->reg_num == r1->reg_num ? CMP_MATCH | CMP_STOP :0);
}
static void  modbus_reg_destructor(void* obj)
{
	struct modbus_reg * reg =obj;

   	   ao2_ref(reg->idx_num,-1);
}
int reg_cmp(void *obj,void* arg, int flag)
{
	struct modbus_reg *r =obj;
    return(r->reg_num == (int)arg ? CMP_MATCH | CMP_STOP :0);
}
struct modbus_reg * find_reg(struct ao2_container * cont, int idx)
{
  struct modbus_reg *f=NULL;
	   f=ao2_callback(cont,0,reg_cmp,(void*)idx);
	  return(f);
}

static struct modbus_reg* modbus_add_reg(struct ao2_container * cont, const char *data )
{
struct modbus_reg * reg =NULL;
int t =atoi(data);

   reg= find_reg(cont,t);
   if(reg == NULL){
	reg=ao2_alloc(sizeof(*reg),modbus_reg_destructor);
	if(reg){
	  reg->idx_num=ao2_container_alloc(16,modbus_bits_hash,modbus_bits_cmp);
	  reg->reg_num=t & 0xFFFF;
      ao2_link(cont,reg);
//      ast_verbose("-------modbus_reg = %d\n",reg->reg_num);
	}
   }
   return reg;
}


static int parse_bit(char *bit,struct modbus_reg* mreg)
{
struct modbus_idxbit * bits =NULL;
char *c;
int t ;

     while(1){
   		if ((c = strchr(bit, ','))) {
 			*c=0;
 			t=atoi(bit);
 			bits=find_bit(mreg->idx_num,t);
 			if(bits == NULL){
              bits=ao2_alloc(sizeof(*bits),modbus_bits_destructor);
              if(bits){
//               ast_verbose("-------bits = %d;num = %s\n",atoi(bit),c+1);
		       bits->bit=t & 0xF;
		       if(  *(c+strlen(c+1)) == ')') *(c+strlen(c+1))=0;
		       bits->num=ast_str_create(strlen(c+1));
	           ast_str_set(&bits->num,0,c+1);
	           ao2_link(mreg->idx_num,bits);
	          }
	        }
	        else{
	          ao2_ref(bits, -1);
	        }
	        bit=c+1;
        }
        else return 0;
     }
 return 0;
}


static int parse_reg(char *reg,struct modbus_reg* mreg)
{
char *c;
    while(1){
   		if ((c = strchr(reg, '|'))) {
			*c=0;
            parse_bit(reg,mreg);
            reg=c+1;
        }
        else {
          parse_bit(reg,mreg);
          return 0;
        }
    }
   return 0;
}
void register_show(struct modbus_reg * r,struct ast_cli_args *a)
{

uint8_t i;
uint16_t t=r->reg_dat;
struct modbus_idxbit *b=NULL;

  ast_cli(a->fd,"----------------------------------------------------------------SINGLE REGISTER [ address=%d]---------------------------------------------------------------------\n",r->reg_num);
  ast_cli(a->fd,"\n");
  ast_cli(a->fd,"%-10s","bit");
  for ( i =0;i<16;i++){
    ast_cli(a->fd,"%-10d",i);
  }
  ast_cli(a->fd,"\n");
  ast_cli(a->fd,"%-10s","num page");
  for ( i =0;i<16;i++){ // проверяем каждый бит
	    b=find_bit(r->idx_num,i);
	    if(b !=NULL){
	    	ast_cli(a->fd,"%-10s",b->num->str);
	      ao2_ref(b, -1);
	    }
        else ast_cli(a->fd,"%-10s","-");
  }
  ast_cli(a->fd,"\n");
  ast_cli(a->fd,"%-10s","state");
  for ( i =0;i<16;i++){ // проверяем каждый бит
		if((t & (1<<i)))  ast_cli(a->fd,"%-10d",1);
		else ast_cli(a->fd,"%-10d",0);
  }
  ast_cli(a->fd,"\n");
}

void modbus_show(struct ast_cli_args *a)
{
	struct modbus_reg * reg= NULL;
    struct ao2_iterator i;
    struct modbus_pvt *mp;
    uint8_t t;

	ast_cli(a->fd,"MODBUS TCP [version =%s ; port=%d ;registr=%d]\n",MODBUS_VERSION,modbus_port,ao2_container_count(mregisters));
     ao2_lock(mregisters);
     i=ao2_iterator_init(mregisters,0);
      while((reg=ao2_iterator_next(&i))){
    	//	ast_debug(2," ----registr=%d[data=0x%X(%d)]\n",reg->reg_num,reg->reg_dat,reg->reg_dat);
    	 /*   ast_verbose("----registr=%d[data=0x%X(%d)]\n",reg->reg_num,reg->reg_dat,reg->reg_dat);
    	    ast_verbose("bit       |number \n");
       	    j=ao2_iterator_init(reg->idx_num,0);
    	    while((bit=ao2_iterator_next(&j))){
    	            ast_verbose("%-10d|%s\n",bit->bit,bit->num->str);
    	        	ao2_ref(bit, -1);
    	    }
    	  */
    	   register_show(reg,a);
    	   ao2_ref(reg, -1);
    //	   ao2_iterator_destroy(&j);

      }
      ao2_iterator_destroy(&i);
      ao2_unlock(mregisters);
      ao2_lock(pvts);
      i=ao2_iterator_init(pvts,0);
      t=ao2_container_count(pvts);
      if(t)
       ast_cli(a->fd,"-------------------------------------------------------MODBUS ACTIV CALLS [count=%d]---------------------------------------------------------------------------\n",t);
      while((mp=ao2_iterator_next(&i))){
    	    ast_cli(a->fd,"----calls=[name=%s,page=%s,ip=%s]\n",mp->name,mp->page,mp->ip);
            ao2_ref(mp, -1);

      }
      ao2_iterator_destroy(&i);
      ao2_unlock(pvts);
}

int modbus_load_cfg(const char *name,int reload)
{
	struct ast_config *cfg;
	struct ast_variable *v;
	struct ast_flags config_flags = { reload == 1 ? CONFIG_FLAG_FILEUNCHANGED : 0 };
    struct modbus_reg* reg =NULL;
    char *c;

	cfg = ast_config_load(name,config_flags);
	if( !cfg ) {
		ast_log(LOG_ERROR, "Unable to load config %s\n",name);
		return -1;
	}
	else if( cfg == CONFIG_STATUS_FILEUNCHANGED ) {
		ast_verbose( "Config file is unchanged.\n");
		return 1;
	}
	else if( cfg == CONFIG_STATUS_FILEINVALID ) {
		ast_log(LOG_ERROR, "Contents of %s are invalid and cannot be parsed\n", name );
		return 1;
	}
	modbus_port=STANDARD_MODBUS_PORT;
	for( v=ast_variable_browse(cfg,"general"); v; v=v->next ) {
		if (!strcasecmp(v->name, "port")) {
			modbus_port = atoi(v->value);
//			ast_verbose("-------modbus_port = %d\n",modbus_port);
		}
     }
	mregisters=ao2_container_alloc(MODBUS_MAX_REGISER,modbus_reg_hash,modbus_reg_cmp);
	ao2_lock(mregisters);
	for( v=ast_variable_browse(cfg,"registers"); v; v=v->next ) {
		// init numbers
	   if (!strcasecmp(v->name, "reg")) {
   		if ((c = strchr(v->value, '('))) {
			*c=0;
            if(MODBUS_MAX_REGISER > ao2_container_count(mregisters)){
			 reg=modbus_add_reg(mregisters,v->value);
             if(reg) parse_reg(c+1,reg);
            }
            else ast_log(LOG_ERROR, "Unable to create registers (max=%d)\n", MODBUS_MAX_REGISER );

        }
       }
     }
	ao2_unlock(mregisters);

//	ast_log(LOG_WARNING, "-----4'\n");
//	ao2_lock(pvts);
	pvts=ao2_container_alloc(32,pvt_hash_cb,pvt_cmp_cb);
//	ao2_unlock(pvts);

//	modbus_show();
	return 0;
}
static void	modbus_data_destructor()
{
//		ast_log(LOG_WARNING, "-----1'\n");
	 if(threadt) ao2_ref(threadt,-1);
//		ast_log(LOG_WARNING, "-----2'\n");
	if(mregisters)ao2_ref(mregisters,-1);
//		ast_log(LOG_WARNING, "-----3'\n");
	if(pvts)ao2_ref(pvts,-1);

}
/*! \brief Function called when our module is loaded */
static int load_module(void)
{
	if (!(modbus_tech.capabilities = ast_format_cap_alloc(0))) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_format_cap_add_all(modbus_tech.capabilities);

    threadt = ao2_t_container_alloc(HASH_MODBUS_SIZE, threadt_hash_cb, threadt_cmp_cb, "allocate threadt table");
	if ( !threadt) {
		ast_log(LOG_ERROR, "Unable to create primary MODBUS container(s)\n");
		return AST_MODULE_LOAD_FAILURE;
	}
    if(modbus_load_cfg(MODBUS_FILE_CONFIG,0)){
		ast_log(LOG_ERROR, "Load config failure\n");
		return AST_MODULE_LOAD_DECLINE;
    }
//	ast_log(LOG_WARNING, "Load_module Modbus'\n");

/* Reset IP addresses  */
	ast_sockaddr_parse(&bindaddr, "0.0.0.0:0", 0);

	/* Start TCP server */
    if (ast_sockaddr_isnull(&modbus_tcp_desc.local_address)) {
		ast_sockaddr_copy(&modbus_tcp_desc.local_address, &bindaddr);
	}
	if (!ast_sockaddr_port(&modbus_tcp_desc.local_address)) {
		ast_sockaddr_set_port(&modbus_tcp_desc.local_address, modbus_port);
	}
//	ast_log(LOG_WARNING, "Setting TCP socket address to %s\n",
//				  ast_sockaddr_stringify(&modbus_tcp_desc.local_address));

	ast_tcptls_server_start(&modbus_tcp_desc);
	if (modbus_tcp_desc.accept_fd == -1) {
		/* TCP server start failed. Tell the admin */
		ast_log(LOG_ERROR, "TCP Server start failed. Not listening on TCP socket.\n");
	} else {
		ast_log(LOG_NOTICE, "Modbus TCP server started( %s)\n",ast_sockaddr_stringify(&modbus_tcp_desc.local_address));
	}
	if(ast_channel_register(&modbus_tech)){
		ast_log(LOG_ERROR, "Register Modbus failure\n");
		return AST_MODULE_LOAD_DECLINE;
	}
    ast_cli_register_multiple(modbus_cli, ARRAY_LEN(modbus_cli));
	return AST_MODULE_LOAD_SUCCESS;
}


/*! \brief Function called when our module is unloaded */
static int unload_module(void)
{
	ast_channel_unregister(&modbus_tech);
	modbus_tech.capabilities = ast_format_cap_destroy(modbus_tech.capabilities);
	ast_tcptls_server_stop(&modbus_tcp_desc);

	/* Kill all existing TCP/TLS threads */
	struct ao2_iterator i = ao2_iterator_init(threadt, 0);
	struct modbus_threadinfo *th;
		while ((th = ao2_t_iterator_next(&i, "iterate through tcp threads for 'sip show tcp'"))) {
			pthread_t thread = th->threadid;
			th->stop = 1;
			pthread_kill(thread, SIGURG);
			ao2_t_ref(th, -1, "decrement ref from iterator");
		}
		ao2_iterator_destroy(&i);

	 modbus_data_destructor();
     ast_cli_unregister_multiple(modbus_cli, ARRAY_LEN(modbus_cli));

	return 0;
}

static void modbus_reload()
{
	ast_log(LOG_NOTICE,"Reload_module Modbus\n");

	ast_tcptls_server_stop(&modbus_tcp_desc);

	/* Kill all existing TCP/TLS threads */
//	if(threadt){
	  struct ao2_iterator i = ao2_iterator_init(threadt, 0);
	  struct modbus_threadinfo *th;
		while ((th = ao2_t_iterator_next(&i, "iterate through tcp threads for 'sip show tcp'"))) {
			pthread_t thread = th->threadid;
			th->stop = 1;
			pthread_kill(thread, SIGURG);
			ao2_t_ref(th, -1, "decrement ref from iterator");
		}
		ao2_iterator_destroy(&i);
//	}
//	modbus_data_destructor();
	if(modbus_load_cfg(MODBUS_FILE_CONFIG,0)){
		    if(mregisters)ao2_ref(mregisters,-1);
			ast_log(LOG_ERROR, "Load config failure\n");
			return;
	}

	/* Reset IP addresses  */
		ast_sockaddr_parse(&bindaddr, "0.0.0.0:0", 0);

		/* Start TCP server */
		ast_sockaddr_copy(&modbus_tcp_desc.old_address, &bindaddr);
		ast_sockaddr_copy(&modbus_tcp_desc.local_address, &bindaddr);
		ast_sockaddr_set_port(&modbus_tcp_desc.local_address, modbus_port);

		ast_tcptls_server_start(&modbus_tcp_desc);
		if (modbus_tcp_desc.accept_fd == -1) {
			/* TCP server start failed. Tell the admin */
			ast_log(LOG_ERROR, "Modbus TCP Server start failed. Not listening on TCP socket.\n");
		} else {
			ast_log(LOG_NOTICE, "Modbus TCP server started( %s)\n",ast_sockaddr_stringify(&modbus_tcp_desc.local_address));
		}
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Modbus Channel",
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);

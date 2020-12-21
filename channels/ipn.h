/*
 * ipn.h
 *
 *  Created on: Jul 3, 2013
 *      Author: forint
 */

#ifndef IPN_H_
#define IPN_H_

#define SETTING_MAX_LEN 32
#define NUMBER_MAX_LEN 16
#define IPN_GROUP_MAX  255

#define IPN_RTP_HEADER_LEN 12
#define IPN_RTP_EXT_LEN 16

#define IPN_FRAME_DATA_LEN 320
#define IPN_FRAME_DATA_LEN 320


#define DEFAULT_BIND_ADDR "0.0.0.0:0"
#define DEFAULT_IGMP_ADDR "224.0.1.11"
#define DEFAULT_STATUS_PORT 21332
#define DEFAULT_GROUPS_PORT 6000
#define DEFAULT_USERS_PORT 5000


enum subscriber_type {
	SUBSCRIBER_USER	=	0,
	SUBSCRIBER_GROUP =	 1,
};

enum direction_call {
	FROM_AICS	=	0,
	TO_AICS =	 1,
};
enum ellan_state {
 ELLANSTAT_IDLE= 0,
 ELLANSTAT_BUSY = 1,
 ELLANSTAT_AFROM =2,
 ELLANSTAT_SCONN =3,
 ELLANSTAT_ATO =4,
 ELLANSTAT_DTO =5,
 ELLANSTAT_DISC =6,
};

#define DEFAULT_TOS_IPN   0      /*!< Call signalling packets should be marked as DSCP CS3, but the default is 0 to be compatible with previous versions. */
#define DEFAULT_COS_IPN   4      /*!< Level 2 class of service for IPN signalling */


#define IPN_MAX_SUBSCRIBER 100
#define AICS_MAX_SUBSCRIBER 100

#endif /* IPN_H_ */

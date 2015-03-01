/*
 * q2pc_protocol.h
 *
 *  Created on: Apr 10, 2014
 *      Author: mgrosvenor
 */

#ifndef Q2PC_PROTOCOL_H_
#define Q2PC_PROTOCOL_H_

#include "../../deps/chaste/chaste.h"

typedef enum {
    q2pc_lost_msg = -1,
    q2pc_request_msg = 0,
    q2pc_vote_yes_msg,
    q2pc_vote_no_msg,
    q2pc_commit_msg,
    q2pc_cancel_msg,
    q2pc_ack_msg,
    q2pc_con_msg
} q2pc_msg_type_t;

typedef struct __attribute__((__packed__)) {
    i16 type;
    i16 src_hostid;
    i16 c_rto;
    i16 s_rto;
    i64 ts;

} q2pc_msg;

#endif /* Q2PC_PROTOCOL_H_ */

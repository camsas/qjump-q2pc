/*
 * q2pc_transport.h
 *
 *  Created on: Apr 9, 2014
 *      Author: mgrosvenor
 */

#ifndef Q2PC_TRANSPORT_H_
#define Q2PC_TRANSPORT_H_

#include "../../deps/chaste/chaste.h"
#include "conn_array.h"
#include "conn_vector.h"


typedef enum { udp_ln = 0, tcp_ln, rdp_ln, udp_qj } transport_e;

typedef struct {
    transport_e type;
    i64 qjump_epoch;
    i64 qjump_limit;
    u16 port;
    bool server;
    char* ip;
    i64 client_count;
    i64 client_id;
    char* bcast;
    char* iface;
    i64 rto_us;
    i64 msize;

} transport_s;


typedef struct q2pc_trans_conn_s {
    int (*beg_read)(struct q2pc_trans_conn_s* this, char** data, i64* len_o);
    int (*end_read)(struct q2pc_trans_conn_s* this);

    int (*beg_write)(struct q2pc_trans_conn_s* this, char** data, i64* len_o);
    int (*end_write)(struct q2pc_trans_conn_s* this, i64 len);

    void (*delete)(struct q2pc_trans_conn_s* this);

    void* priv;
} q2pc_trans_conn;


typedef struct q2pc_trans_s {
    int (*connect)(struct q2pc_trans_s* this, q2pc_trans_conn* conn);

    void (*delete)(struct q2pc_trans_s* this);

    void* priv;
} q2pc_trans;


q2pc_trans* trans_factory(const transport_s* transport);

#endif /* Q2PC_TRANSPORT_H_ */

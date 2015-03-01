/*
 * q2pc_trans_rudp.c
 *
 *  Created on: Apr 12, 2014
 *      Author: mgrosvenor
 */


/*
 * q2pc_trans_server.c
 *
 *  Created on: Apr 9, 2014
 *      Author: mgrosvenor
 */
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>

#include "q2pc_trans_rudp.h"
#include "q2pc_trans_udp.h"
#include "conn_vector.h"
#include "../errors/errors.h"
#include "../protocol/q2pc_protocol.h"

typedef struct {
    q2pc_trans_conn base;
    bool is_server;
    volatile i64 seq_no;

    char* read_data;
    i64 read_data_len;

    char* write_data;

    pthread_mutex_t mutex;

    struct timeval ts_start;
    struct timeval ts_now;
    i64 ts_start_us;
    i64 ts_now_us;

    bool ack_outstanding;
    i64 current_seq;

    i64 rto_timeout_us;

} q2pc_rudp_conn_priv;


#define BARRIER()  __asm__ volatile("" ::: "memory")
#define PAUSE()    __asm__ volatile("pause")



static int conn_beg_read(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_rudp_conn_priv* priv = (q2pc_rudp_conn_priv*)this->priv;
    //pthread_mutex_lock(&priv->mutex);

    //There is already data waiting, so exit early
    if(priv->read_data && priv->read_data_len){
        (*data_o) = priv->read_data + sizeof(priv->seq_no);
        (*len_o)  = priv->read_data_len - sizeof(priv->seq_no);
        //pthread_mutex_unlock(&priv->mutex);
        return Q2PC_ENONE;
    }

    int result = priv->base.beg_read(&priv->base,data_o, len_o);
    if(result){

        if(result == Q2PC_EFIN){
            ch_log_debug3("RUDP beg read EFIN\n");
        }

        if(result != Q2PC_EAGAIN && result != Q2PC_EFIN){
            ch_log_warn("Base stream returned error %li\n", result);
        }

        //ch_log_warn("No data exit\n");
        priv->base.end_read(&priv->base);
        //pthread_mutex_unlock(&priv->mutex);
        return result;
    }


    i64 seq_no = *(i64*)(*data_o);
    if(priv->is_server){
        ch_log_debug3("Server got message with seq_no=%li\n", seq_no);
        if(seq_no != priv->seq_no){
            ch_log_debug1("Server dropping message with seq_no %li != %li\n", seq_no, priv->seq_no);
            priv->base.end_read(&priv->base);
            //pthread_mutex_unlock(&priv->mutex);
            return Q2PC_EAGAIN;
        }

        ch_log_debug3("Seq no is now %li --> %li\n", priv->seq_no, priv->seq_no + 1);
        priv->seq_no++;
        BARRIER(); //Make this thread safe so that every one sees this update
    }
    else{
        ch_log_debug3("Client got message with seq_no=%li\n", seq_no);

        if(seq_no <= priv->seq_no){
            ch_log_debug1("Client dropping message with seq_no=%li <= %li\n", seq_no, priv->seq_no);
            priv->base.end_read(&priv->base);
            //pthread_mutex_unlock(&priv->mutex);
            return Q2PC_EAGAIN;
        }

        ch_log_debug3("Seq no is now %li --> %li\n", priv->seq_no, seq_no);
        priv->seq_no = seq_no;
        BARRIER(); //Make this thread safe so that every one sees this update

    }

    priv->read_data     = (*data_o) ;
    priv->read_data_len = (*len_o)  ;

    (*data_o) = priv->read_data + sizeof(priv->seq_no);
    (*len_o)  = priv->read_data_len - sizeof(priv->seq_no);

    //pthread_mutex_unlock(&priv->mutex);
    return Q2PC_ENONE;


}

static int conn_end_read(struct q2pc_trans_conn_s* this)
{
    q2pc_rudp_conn_priv* priv = (q2pc_rudp_conn_priv*)this->priv;
    int result = priv->base.end_read(&priv->base);

    priv->read_data     = NULL;
    priv->read_data_len = 0;

    return result;
}


static int conn_beg_write(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_rudp_conn_priv* priv = (q2pc_rudp_conn_priv*)this->priv;
    int result = priv->base.beg_write(&priv->base, data_o, len_o);
    if(result){
        ch_log_warn("Base stream returned error %li\n", result);
        return result;
    }


    i64* seq_no = (i64*)(*data_o);
    *seq_no = priv->seq_no;

    if(priv->is_server){
        ch_log_debug3("Server made message with seq_no=%li\n", *seq_no);
    }
    else{
        ch_log_debug3("Client made message with seq_no=%li\n", *seq_no);
    }


    (*data_o) += sizeof(priv->seq_no);
    (*len_o)  -= sizeof(priv->seq_no);
    priv->write_data = (*data_o);


    return Q2PC_ENONE;
}


static int conn_end_write(struct q2pc_trans_conn_s* this, i64 len)
{
    q2pc_rudp_conn_priv* priv = (q2pc_rudp_conn_priv*)this->priv;

    if(!priv->ack_outstanding){
        ch_log_debug3("Committing write to base stream\n");
        int result = priv->base.end_write(&priv->base, len + sizeof(priv->seq_no));

        if(result){
            ch_log_warn("Base stream returned error %li\n", result);
            return result;
        }

        gettimeofday(&priv->ts_start, NULL);
        priv->ts_start_us = priv->ts_start.tv_sec * 1000 * 1000 + priv->ts_start.tv_usec;
        ch_log_debug3("Time now = %li\n", priv->ts_start_us);
        priv->current_seq = priv->seq_no;

        priv->ack_outstanding = true;

    }

    //Try to stimulate a a seq_no change
    if(priv->is_server) {
        char* rd_data;
        i64 rd_len;
        int result = conn_beg_read(this,&rd_data,&rd_len);
        if(result == Q2PC_EFIN){
            return result;
        }
    }

    if(priv->current_seq != priv->seq_no){
        ch_log_debug3("Got ack for seq=%li\n", priv->current_seq);
        priv->ack_outstanding = false;
        return Q2PC_ENONE; //Winner!
    }

    gettimeofday(&priv->ts_now, NULL);
    priv->ts_now_us = priv->ts_now.tv_sec * 1000 * 1000 + priv->ts_now.tv_usec;
    if(priv->ts_now_us < priv->ts_start_us + priv->rto_timeout_us){
        return Q2PC_EAGAIN;
    }

    ch_log_debug3("Time now %li > %li (diff=%li > %li)\n", priv->ts_now_us, priv->ts_start_us, priv->ts_now_us - priv->ts_start_us, priv->rto_timeout_us);

    //ch_log_warn("Retransmit timeout fired on seq_no=%lu\n", priv->seq_no);

    //XXX HACK!
    if(priv->is_server){
        ((q2pc_msg*)priv->write_data)->c_rto++;
        ch_log_debug3("Set c_rto to %i\n", ((q2pc_msg*)priv->write_data)->c_rto) ;
    }
    else{
        ((q2pc_msg*)priv->write_data)->s_rto++;
        ch_log_debug3("Set s_rto to %i\n", ((q2pc_msg*)priv->write_data)->s_rto) ;
    }

    int result = priv->base.end_write(&priv->base, len + sizeof(priv->seq_no));
    if(result){

        if(result == Q2PC_EFIN){
            ch_log_debug3("RUDP end write EFIN\n");
            return Q2PC_EFIN;
        }

        ch_log_warn("Base stream returned error %i\n", result);
    }

    gettimeofday(&priv->ts_start, NULL);
    priv->ts_start_us = priv->ts_start.tv_sec * 1000 * 1000 + priv->ts_start.tv_usec;
    ch_log_debug3("Time now = %li\n", priv->ts_start_us);

    return Q2PC_RTOFIRED;
}


static void conn_delete(struct q2pc_trans_conn_s* this)
{
    if(this){
        if(this->priv){
            q2pc_rudp_conn_priv* priv = (q2pc_rudp_conn_priv*)this->priv;
            priv->base.delete(&priv->base);
            free(this->priv);
        }

        //XXX HACK!
        //free(this);
    }
}



/***************************************************************************************************************************/

typedef struct {

    transport_s transport;
    q2pc_trans* base;
} q2pc_rudp_priv;



#define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because it feels right
static q2pc_rudp_conn_priv* init_new_conn(q2pc_trans_conn* conn)
{
    q2pc_rudp_conn_priv* new_priv = calloc(1,sizeof(q2pc_rudp_conn_priv));
    if(!new_priv){
        ch_log_fatal("Malloc failed!\n");
    }

    conn->priv      = new_priv;
    conn->beg_read  = conn_beg_read;
    conn->end_read  = conn_end_read;
    conn->beg_write = conn_beg_write;
    conn->end_write = conn_end_write;
    conn->delete    = conn_delete;

    return new_priv;
}



//Wait for all clients to connect
static int doconnect(struct q2pc_trans_s* this, q2pc_trans_conn* conn)
{
    q2pc_rudp_priv* trans_priv = (q2pc_rudp_priv*)this->priv;
    q2pc_rudp_conn_priv* conn_priv = (q2pc_rudp_conn_priv*)conn->priv;

    if(!conn_priv){
        conn_priv                   = init_new_conn(conn);
        conn_priv->is_server        = !trans_priv->transport.server;
        conn_priv->seq_no           = conn_priv->is_server ? 0 : -1; //Set to -1 for clients
        conn_priv->read_data        = NULL;
        conn_priv->read_data_len    = 0;
        conn_priv->rto_timeout_us   = trans_priv->transport.rto_us;
        conn_priv->ack_outstanding  = false;

        conn->priv           = conn_priv;

        if(trans_priv->base->connect(trans_priv->base,&conn_priv->base)){
            ch_log_fatal("Could not create UDP base for RUDP\n");
        }
    }

    return Q2PC_ENONE;
}


static void serv_delete(struct q2pc_trans_s* this)
{
    if(this){

        if(this->priv){
            q2pc_rudp_priv* priv = (q2pc_rudp_priv*)this->priv;
            priv->base->delete(priv->base);
            free(this->priv);
        }

        free(this);
    }

}


static void init(q2pc_rudp_priv* priv)
{

    ch_log_debug1("Constructing RUDP transport\n");
    priv->base = q2pc_udp_construct(&priv->transport);
    ch_log_debug1("Done constructing RUDP transport\n");

}


q2pc_trans* q2pc_rudp_construct(const transport_s* transport)
{
    q2pc_trans* result = (q2pc_trans*)calloc(1,sizeof(q2pc_trans));
    if(!result){
        ch_log_fatal("Could not allocate RUDP server structure\n");
    }

    q2pc_rudp_priv* priv = (q2pc_rudp_priv*)calloc(1,sizeof(q2pc_rudp_priv));
    if(!priv){
        ch_log_fatal("Could not allocate RUDP server private structure\n");
    }

    result->priv          = priv;
    result->connect       = doconnect;
    result->delete        = serv_delete;
    memcpy(&priv->transport,transport, sizeof(transport_s));
    init(priv);


    return result;
}

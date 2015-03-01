/*
 * q2pc_trans_qj.c
 *
 *  Created on: Apr 12, 2014
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

#include "q2pc_trans_qj.h"
#include "conn_vector.h"
#include "../errors/errors.h"
#include "../protocol/q2pc_protocol.h"

typedef struct {
    int wr_fd; //Writing file descriptor
    int rd_fd; //Reading file descriptor

    //For the reader
    void* read_buffer;
    i64   read_buffer_used;
    i64   read_buffer_size;

    //For the writer
    void* write_buffer;
    i64   write_buffer_used;
    i64   write_buffer_size;

} q2pc_qj_conn_priv;



static int conn_beg_read(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_qj_conn_priv* priv = (q2pc_qj_conn_priv*)this->priv;
    if( priv->read_buffer && priv->read_buffer_used){
        return Q2PC_ENONE;
    }

    int result = read(priv->rd_fd, priv->read_buffer, priv->read_buffer_size);
    if(result < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            return Q2PC_EAGAIN; //Reading would have blocked, we don't want this
        }

        ch_log_fatal("qj read failed on fd=%i - %s\n",priv->rd_fd,strerror(errno));
    }

    if(result == 0){
        return Q2PC_EFIN;
    }

    priv->read_buffer_used = result;

    *data_o = priv->read_buffer;
    *len_o  = priv->read_buffer_used;
    ch_log_debug3("Got %li bytes\n", priv->read_buffer_used);


    return Q2PC_ENONE;
}

static int conn_end_read(struct q2pc_trans_conn_s* this)
{
    q2pc_qj_conn_priv* priv = (q2pc_qj_conn_priv*)this->priv;
    priv->read_buffer_used = 0;
    return 0;
}



static int conn_beg_write(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_qj_conn_priv* priv = (q2pc_qj_conn_priv*)this->priv;
    *data_o = priv->write_buffer;
    *len_o  = priv->write_buffer_size;
    return 0;
}


static int conn_end_write(struct q2pc_trans_conn_s* this, i64 len)
{
    q2pc_qj_conn_priv* priv = (q2pc_qj_conn_priv*)this->priv;
    char* data = priv->write_buffer;

    if(len > priv->write_buffer_size){
        ch_log_fatal("Error: Wrote more data than the buffer could handle. Memory corruption is likely\n ");
    }

    while(len > 0){
        i64 written =  write(priv->wr_fd, data ,len);
        if(written < 0){
            ch_log_fatal("QJ write failed: %s\n",strerror(errno));
        }
        data += written;
        len -= written;
    }

    return 0;

}


static void conn_delete(struct q2pc_trans_conn_s* this)
{
    if(this){
        if(this->priv){
            q2pc_qj_conn_priv* priv = (q2pc_qj_conn_priv*)this->priv;
            if(priv->read_buffer){ free(priv->read_buffer); }
            //if(priv->write_buffer){ free(priv->write_buffer); } --Not necessary since r+w are allocated together
            free(this->priv);
        }

        //XXX HACK!
        //free(this);
    }
}



/***************************************************************************************************************************/

typedef struct {
    int fd;
    transport_s transport;

    i64 connections;

} q2pc_qj_priv;


#define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because it feels right
static q2pc_qj_conn_priv* new_conn_priv()
{
    q2pc_qj_conn_priv* new_priv = calloc(1,sizeof(q2pc_qj_conn_priv));
    if(!new_priv){
        ch_log_fatal("Malloc failed!\n");
    }

    void* read_buff = calloc(2,BUFF_SIZE);
    if(!read_buff){
        ch_log_fatal("Malloc failed!\n");
    }
    new_priv->read_buffer = read_buff;
    new_priv->read_buffer_size = BUFF_SIZE;

    void* write_buff = (char*)read_buff + BUFF_SIZE;
    new_priv->write_buffer = write_buff;
    new_priv->write_buffer_size = BUFF_SIZE;


    return new_priv;

}

static q2pc_qj_conn_priv* init_new_conn(q2pc_trans_conn* conn)
{
    q2pc_qj_conn_priv* new_priv = new_conn_priv();


    conn->priv      = new_priv;
    conn->beg_read  = conn_beg_read;
    conn->end_read  = conn_end_read;
    conn->beg_write = conn_beg_write;
    conn->end_write = conn_end_write;
    conn->delete    = conn_delete;

    return new_priv;
}


static void safe_connect(int fd, struct sockaddr_in* addr)
{
    ch_log_debug3("Connecting on %i port=%i\n", fd, ntohs(addr->sin_port));
    if( connect(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) ){
        ch_log_fatal("QJ connect failed: %s\n",strerror(errno));
    }

}


static void safe_wait_bind(int fd, struct sockaddr_in* addr)
{

    ch_log_debug3("Binding on %i port=%i\n", fd, ntohs(addr->sin_port));

    if(bind(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) ){
        uint64_t i = 0;

        //Will wait up to two minutes trying if the address is in use.
        //Helpful for quick restarts of apps as Linux keeps some state
        //around for a while.
        const int64_t seconds_per_try = 5;
        const int64_t seconds_total = 120;
        for(i = 0; i < seconds_total / seconds_per_try && errno == EADDRINUSE; i++){
            ch_log_debug1("%i] %s --> sleeping for %i seconds...\n",i, strerror(errno), seconds_per_try);
            sleep(seconds_per_try);
            bind(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
        }

        if(errno){
            ch_log_fatal("QJ server bind failed: %s\n",strerror(errno));
        }
        else{
            ch_log_debug1("Successfully bound after delay.\n");
        }
    }

}

static int new_socket()
{
    int sock_fd = socket(AF_INET,SOCK_DGRAM,0);
    if (sock_fd < 0 ){
        ch_log_fatal("Could not create QJ socket (%s)\n", strerror(errno));
    }

    int reuse_opt = 1;
    if(setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_opt, sizeof(int)) < 0) {
        ch_log_fatal("QJ set reuse address failed: %s\n",strerror(errno));
    }

    int priority = 7;
    if(setsockopt(sock_fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(int)) < 0) {
        ch_log_fatal("QJ set priority address failed: %s\n",strerror(errno));
    }


    int flags = 0;
    flags |= O_NONBLOCK;
    if( fcntl(sock_fd, F_SETFL, flags) == -1){
        ch_log_fatal("Could not set non-blocking on fd=%i: %s\n",sock_fd,strerror(errno));
    }

    return sock_fd;

}



//Wait for all clients to connect
static int doconnect(struct q2pc_trans_s* this, q2pc_trans_conn* conn)
{
    q2pc_qj_priv* trans_priv = (q2pc_qj_priv*)this->priv;
    q2pc_qj_conn_priv* conn_priv = (q2pc_qj_conn_priv*)conn->priv;

    if(!conn_priv){

        q2pc_qj_conn_priv* new_priv = init_new_conn(conn);

        int sock_rd_fd = new_socket();
        int sock_wr_fd = new_socket();

        struct sockaddr_in addr;
        memset(&addr,0,sizeof(addr));
        addr.sin_family      = AF_INET;

        if(trans_priv->transport.server){
            //Listen to any address, on the client port number
            trans_priv->connections++;

            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port        = htons(trans_priv->transport.port + trans_priv->connections);
            safe_wait_bind(sock_rd_fd,&addr);
            new_priv->rd_fd = sock_rd_fd;

            //Send to the client(s) on the broadcast port
            addr.sin_addr.s_addr = inet_addr(trans_priv->transport.bcast);
            addr.sin_port        = htons(trans_priv->transport.port);

            int broadcastEnable=1;
            if( setsockopt(sock_wr_fd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable)) ){
                ch_log_fatal("Could not set broadcast on fd=%i: %s\n",sock_wr_fd,strerror(errno));
            }

            ch_log_debug2("Binding to interface name=%s\n", trans_priv->transport.iface);
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", trans_priv->transport.iface );
            if( setsockopt(sock_wr_fd, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) ){
                ch_log_fatal("Could not set interface on fd=%i: %s\n",sock_wr_fd,strerror(errno));
            }

            safe_connect(sock_wr_fd,&addr);

        }
        else{

            //Listen to any address, on the server broadcast port
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port        = htons(trans_priv->transport.port);
            safe_wait_bind(sock_rd_fd,&addr);

            //Send to the server on the server port
            addr.sin_addr.s_addr = inet_addr(trans_priv->transport.ip);
            addr.sin_port        = htons(trans_priv->transport.port + trans_priv->transport.client_id);
            safe_connect(sock_wr_fd,&addr);
        }

        new_priv->rd_fd = sock_rd_fd;
        new_priv->wr_fd = sock_wr_fd;

        conn->priv = new_priv;

    }

    return Q2PC_ENONE;
}


static void serv_delete(struct q2pc_trans_s* this)
{
    if(this){

        if(this->priv){
            //q2pc_qj_priv* priv = (q2pc_qj_priv*)this->priv;
            free(this->priv);
        }

        free(this);
    }

}


#define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because
static void init(q2pc_qj_priv* priv)
{

    (void)priv;
    ch_log_debug1("Constructing QJ transport\n");

    ch_log_debug1("Done constructing QJ transport\n");

}


q2pc_trans* q2pc_qj_construct(const transport_s* transport)
{
    q2pc_trans* result = (q2pc_trans*)calloc(1,sizeof(q2pc_trans));
    if(!result){
        ch_log_fatal("Could not allocate QJ server structure\n");
    }

    q2pc_qj_priv* priv = (q2pc_qj_priv*)calloc(1,sizeof(q2pc_qj_priv));
    if(!priv){
        ch_log_fatal("Could not allocate QJ server private structure\n");
    }

    result->priv          = priv;
    result->connect       = doconnect;
    result->delete        = serv_delete;
    memcpy(&priv->transport,transport, sizeof(transport_s));
    init(priv);


    return result;
}

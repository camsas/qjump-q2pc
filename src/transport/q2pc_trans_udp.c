/*
 * q2pc_trans_udp.c
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

#include "q2pc_trans_udp.h"
#include "conn_vector.h"
#include "../errors/errors.h"
#include "../protocol/q2pc_protocol.h"

typedef struct {
    int fd; //Reading file descriptor

    //For the reader
    void* read_buffer;
    i64   read_buffer_used;
    i64   read_buffer_size;

    //For the writer
    void* write_buffer;
    i64   write_buffer_used;
    i64   write_buffer_size;

    bool is_connected;
    struct sockaddr_in src_addr;

} q2pc_udp_conn_priv;

//Forward declaration
static void safe_connect(int fd, struct sockaddr_in* addr);


static int conn_beg_read(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_udp_conn_priv* priv = (q2pc_udp_conn_priv*)this->priv;
    if( priv->read_buffer && priv->read_buffer_used){
        return Q2PC_ENONE;
    }

    int result = -1 ;
    if(unlikely(!priv->is_connected)){
        //ch_log_debug3("Connecting with rcv from\n");
        socklen_t addr_len = sizeof(priv->src_addr);
        result = recvfrom(priv->fd,priv->read_buffer, priv->read_buffer_size, 0, (struct sockaddr*)&priv->src_addr, &addr_len);

        if(result > 0){
            safe_connect(priv->fd,&priv->src_addr);
            ch_log_debug3("Connected to %li\n", ntohs(priv->src_addr.sin_port));
            priv->is_connected = true;
        }

    }
    else{
        result = read(priv->fd, priv->read_buffer, priv->read_buffer_size);
        //ch_log_debug3("Read to %i bytes\n",result);
    }

    if(result < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            return Q2PC_EAGAIN; //Reading would have blocked, we don't want this
        }

        if(errno == ECONNREFUSED){
            ch_log_warn("UDP beg read EFIN (%s)\n", strerror(errno));
            return Q2PC_EFIN;
        }

        ch_log_fatal("udp read failed on fd=%i with errno=%i (%s)\n",priv->fd, errno, strerror(errno));
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
    q2pc_udp_conn_priv* priv = (q2pc_udp_conn_priv*)this->priv;
    priv->read_buffer_used = 0;
    return 0;
}



static int conn_beg_write(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_udp_conn_priv* priv = (q2pc_udp_conn_priv*)this->priv;
    *data_o = priv->write_buffer;
    *len_o  = priv->write_buffer_size;
    return 0;
}


static int conn_end_write(struct q2pc_trans_conn_s* this, i64 len)
{
    q2pc_udp_conn_priv* priv = (q2pc_udp_conn_priv*)this->priv;
    char* data = priv->write_buffer;

    if(len > priv->write_buffer_size){
        ch_log_fatal("Error: Wrote more data than the buffer could handle. Memory corruption is likely\n ");
    }

    while(len > 0){
        i64 written =  write(priv->fd, data ,len);
        if(written < 0){

            if(errno == EAGAIN || errno == EWOULDBLOCK){
                continue; //Keep trying until we succeed
            }

            if(errno == ECONNREFUSED){
                ch_log_debug3("UDP end write EFIN\n");
                return Q2PC_EFIN;
            }

            ch_log_warn("UDP write failed with errorno=%i: %s\n", errno, strerror(errno));
            return Q2PC_EFIN;
        }
        data += written;
        len -= written;
    }

    return Q2PC_ENONE;

}


static void conn_delete(struct q2pc_trans_conn_s* this)
{
    if(this){
        if(this->priv){
            q2pc_udp_conn_priv* priv = (q2pc_udp_conn_priv*)this->priv;
            if(priv->read_buffer){ free(priv->read_buffer); }
            //if(priv->write_buffer){ free(priv->write_buffer); } --Not necessary since r+w are allocated together
            free(this->priv);
            close(priv->fd);
        }

        //XXX HACK!
        //free(this);
    }
}



/***************************************************************************************************************************/

typedef struct {
    transport_s transport;
    i64 connections;

} q2pc_udp_priv;




#define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because it feels right
static q2pc_udp_conn_priv* new_conn_priv()
{
    q2pc_udp_conn_priv* new_priv = calloc(1,sizeof(q2pc_udp_conn_priv));
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

static q2pc_udp_conn_priv* init_new_conn(q2pc_trans_conn* conn)
{
    q2pc_udp_conn_priv* new_priv = new_conn_priv();


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
    if( connect(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) ){
        ch_log_fatal("UDP connect failed: %s\n",strerror(errno));
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
            ch_log_fatal("UDP server bind failed: %s\n",strerror(errno));
        }
        else{
            ch_log_debug1("Successfully bound after delay.\n");
        }
    }

}



//Wait for all clients to connect
static int doconnect(struct q2pc_trans_s* this, q2pc_trans_conn* conn)
{
    q2pc_udp_priv* trans_priv = (q2pc_udp_priv*)this->priv;
    q2pc_udp_conn_priv* conn_priv = (q2pc_udp_conn_priv*)conn->priv;

    if(!conn_priv){

        q2pc_udp_conn_priv* new_priv = init_new_conn(conn);

        new_priv->fd = socket(AF_INET,SOCK_DGRAM,0);
        if(new_priv->fd < 0 ){
            ch_log_fatal("Could not create UDP socket (%s)\n", strerror(errno));
        }



        if(trans_priv->transport.server){
            //Listen to any address, on the client port
            memset(&new_priv->src_addr,0,sizeof(new_priv->src_addr));
            new_priv->is_connected = false;

            ch_log_debug3("Binding to %li\n", ntohs(new_priv->src_addr.sin_port));
            new_priv->src_addr.sin_family      = AF_INET;
            new_priv->src_addr.sin_addr.s_addr = INADDR_ANY;
            new_priv->src_addr.sin_port        = htons(trans_priv->transport.port + trans_priv->connections);
            safe_wait_bind(new_priv->fd,&new_priv->src_addr);

            trans_priv->connections++;
        }
        else{
            struct sockaddr_in addr;
            memset(&addr,0,sizeof(addr));
            new_priv->is_connected = true;
            //Listen to any address, on the server broadcast port
            ch_log_debug3("Connecting to %li\n", ntohs(addr.sin_port));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_addr.s_addr = inet_addr(trans_priv->transport.ip);
            addr.sin_port        = htons(trans_priv->transport.port + trans_priv->transport.client_id);

            safe_connect(new_priv->fd,&addr);
        }

        int reuse_opt = 1;
        if(setsockopt(new_priv->fd, SOL_SOCKET, SO_REUSEADDR, &reuse_opt, sizeof(int)) < 0) {
            ch_log_fatal("UDP set reuse address failed: %s\n",strerror(errno));
        }

        int flags = 0;
        flags |= O_NONBLOCK;
        if( fcntl(new_priv->fd, F_SETFL, flags) == -1){
            ch_log_fatal("Could not set non-blocking on fd=%i: %s\n",new_priv->fd,strerror(errno));
        }

        conn->priv = new_priv;

    }

    return Q2PC_ENONE;
}


static void serv_delete(struct q2pc_trans_s* this)
{
    if(this){

        if(this->priv){
            //q2pc_udp_priv* priv = (q2pc_udp_priv*)this->priv;
            free(this->priv);
        }

        free(this);
    }

}


#define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because
static void init(q2pc_udp_priv* priv)
{

    ch_log_debug1("Constructing UDP transport\n");

    //Keep track of port numbers
    priv->connections = 1;

    ch_log_debug1("Done constructing UDP transport\n");

}


q2pc_trans* q2pc_udp_construct(const transport_s* transport)
{
    q2pc_trans* result = (q2pc_trans*)calloc(1,sizeof(q2pc_trans));
    if(!result){
        ch_log_fatal("Could not allocate UDP server structure\n");
    }

    q2pc_udp_priv* priv = (q2pc_udp_priv*)calloc(1,sizeof(q2pc_udp_priv));
    if(!priv){
        ch_log_fatal("Could not allocate UDP server private structure\n");
    }

    result->priv          = priv;
    result->connect       = doconnect;
    result->delete        = serv_delete;
    memcpy(&priv->transport,transport, sizeof(transport_s));
    init(priv);


    return result;
}

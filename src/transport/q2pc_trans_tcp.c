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
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>

#include "q2pc_trans_tcp.h"
#include "conn_vector.h"
#include "../errors/errors.h"
#include "../protocol/q2pc_protocol.h"

typedef struct {
    int fd;

    //For the reader
    void* read_buffer;
    i64   read_buffer_used;
    i64   read_buffer_size;

    //For the writer
    void* write_buffer;
    i64   write_buffer_used;
    i64   write_buffer_size;

    //For the delimiter
    void* delim_buffer;
    i64   delim_buffer_used;
    i64   delim_buffer_size;
    void* delim_result;
    i64   delim_result_len;


} q2pc_tcp_conn_priv;


i64 delimit(char* buff, i64 len);


static int conn_beg_read(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_tcp_conn_priv* priv = (q2pc_tcp_conn_priv*)this->priv;
    if( priv->read_buffer && priv->read_buffer_used){
        return Q2PC_ENONE;
    }

    int result = read(priv->fd, priv->read_buffer, priv->read_buffer_size);
    if(result < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            return Q2PC_EAGAIN; //Reading would have blocked, we don't want this
        }

        if(errno == ECONNREFUSED){
            return Q2PC_EFIN;
        }

        ch_log_fatal("TCP read failed on fd=%i - %s\n",priv->fd,strerror(errno));
    }

    if(result == 0){
        return Q2PC_EFIN;
    }

    priv->read_buffer_used = result;

    *data_o = priv->read_buffer;
    *len_o  = priv->read_buffer_used;

    return Q2PC_ENONE;
}

static int conn_end_read(struct q2pc_trans_conn_s* this)
{
    q2pc_tcp_conn_priv* priv = (q2pc_tcp_conn_priv*)this->priv;
    priv->read_buffer_used = 0;
    return Q2PC_ENONE;
}

static int conn_beg_delimit(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_tcp_conn_priv* priv = (q2pc_tcp_conn_priv*)this->priv;

    if(priv->delim_result && priv->delim_result_len){
        *data_o  = priv->delim_result;
        *len_o   = priv->delim_result_len;

        return Q2PC_ENONE;
    }

    if(priv->delim_buffer_used){
        int64_t delimit_size = delimit(priv->delim_buffer, priv->delim_buffer_used);
        if(delimit_size > 0 && delimit_size <= priv->delim_buffer_used){

            priv->delim_result     = priv->delim_buffer;
            priv->delim_result_len = delimit_size;
            *data_o                = priv->delim_result;
            *len_o                 = priv->delim_result_len;


            return Q2PC_ENONE; //Success!!
        }
    }

    char* data;
    i64 len;
    int result = conn_beg_read(this, &data,&len);
    if(result != Q2PC_ENONE){
        return result;
    }


    ch_log_debug2("Read another %lu bytes to %p\n", len, data);

    while(len > priv->delim_buffer_size - priv->delim_buffer_used){
        ch_log_debug2("Growing working buffer from %lu to %lu\n", priv->delim_buffer_size, priv->delim_buffer_size * 2 );
        priv->delim_buffer_size *= 2;
        priv->delim_buffer = realloc(priv->delim_buffer, priv->delim_buffer_size);
    }

    ch_log_debug2("Adding %lu bytes at %p (offset=%lu)\n",
            len,
            (char*)priv->delim_buffer + priv->delim_buffer_used,
            priv->delim_buffer_used
    );

    memcpy((char*)priv->delim_buffer + priv->delim_buffer_used, data, len);
    priv->delim_buffer_used += len;
    conn_end_read(this);

    int64_t delimit_size = delimit(priv->delim_buffer, priv->delim_buffer_used);
    if(delimit_size > 0 && delimit_size <= priv->delim_buffer_used){
        priv->delim_result     = priv->delim_buffer;
        *data_o                = priv->delim_buffer;
        priv->delim_result_len = delimit_size;
        *len_o                 = delimit_size;

        ch_log_debug2("Found message size=%lu\n", *len_o);
        return Q2PC_ENONE;
    }

    //This is naughty, it says there isn't any data here, try again later, which is kind of true.
    return Q2PC_EAGAIN;
}


//** Warning: This is a complicated function with a lot of edge cases. It's vital to performance and
//            correctness to get this one right!
//
// We enter this function as a result of a successful call to begin delimit. Begin delimit may have succeeded
// for one of two reasons, 1) either new data was read and the delimiter was successful, or, 2) we have
// optimistically found another delimited result in a previous call to end delimit(). In the first case,
// priv->delimit_buffer_used is at least equal to the size of priv->delim_result_len, in the second case,
// priv->delimit_buffer size is smaller.
static int conn_end_delimit(struct q2pc_trans_conn_s* this)
{
    q2pc_tcp_conn_priv* priv = this->priv;
    if(!priv->delim_result){
        return 0;
    }


    char* result_head_next = (char*)priv->delim_result + priv->delim_result_len;

    //Handle case 1)
    //New data was read and the delimiter was successful
    if(priv->delim_result_len <= priv->delim_buffer_used){

        //This is how much is now left over
        priv->delim_buffer_used -= priv->delim_result_len;

        //If there is nothing left over, give up and bug out
        if(!priv->delim_buffer_used){
            priv->delim_result_len= 0;
            priv->delim_result= NULL;
            return 0;
        }

        //Something is left over we can optimistically check if there happens to be another packet ready to go.
        //The reduces the number of memmoves and significantly improves performance especially with TCP where the
        //read size can be *huge*. (>4MB with TSO turned on)
        i64 delimit_size = delimit(result_head_next, priv->delim_buffer_used);
        if(delimit_size && delimit_size <= priv->delim_buffer_used){
            //Ready for the next round with data available! No memmove required!
            priv->delim_result = result_head_next;
            priv->delim_result_len = delimit_size;
            return 0;
        }

        //Nope, ok, bite the bullet and move stuff around. We'll need to call read() and get some more data
        //printf("Doing mem move of %lu from %p to %p (total=%lu)\n", priv->delim_buffer_used, result_head_next, priv->delim_buffer, result_head_next - priv->delim_buffer);
        memmove(priv->delim_buffer, result_head_next, priv->delim_buffer_used);
    }

    //XXX: I can't see a reason why this case would ever trigger. I know it took me a long time to write this, so something must trigger it, but I don't know what it is.
    //Handle case 2)
    //There is some data leftover, but less than the last result buffer size.
    else{

        //Optimistically check if there is a new packet in this left over. Remember, packets are not all the same
        //size, so a samll packet could follow a big packet.
        i64 delimit_size = delimit(result_head_next, priv->delim_buffer_used);
        if(delimit_size && delimit_size <= priv->delim_buffer_used){
            //Ready for the next round with data available! Yay! No memmove required!
            priv->delim_result = result_head_next;
            priv->delim_result_len = delimit_size;
            return 0;
        }

        //Nope, ok, bite the bullet and move stuff around. We'll need to call read() and get some more data
        //printf("Doing mem move of %lu from %p to %p (total=%lu)\n", priv->delim_buffer_used, result_head_next, priv->delim_buffer, result_head_next - priv->delim_buffer);
        memmove(priv->delim_buffer, result_head_next, priv->delim_buffer_used);

    }

    //Ready for the next round, there is no new delimited packet available, so reset everything and get ready for a call to read()
    priv->delim_result_len= 0;
    priv->delim_result= NULL;
    return 0;

}



static int conn_beg_write(struct q2pc_trans_conn_s* this, char** data_o, i64* len_o)
{
    q2pc_tcp_conn_priv* priv = (q2pc_tcp_conn_priv*)this->priv;
    *data_o = priv->write_buffer;
    *len_o  = priv->write_buffer_size;
    return 0;
}


static int conn_end_write(struct q2pc_trans_conn_s* this, i64 len)
{
    q2pc_tcp_conn_priv* priv = (q2pc_tcp_conn_priv*)this->priv;
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
                return Q2PC_EFIN;
            }

            ch_log_warn("TCP write failed: %s\n",strerror(errno));
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
            q2pc_tcp_conn_priv* priv = (q2pc_tcp_conn_priv*)this->priv;
            if(priv->read_buffer){ free(priv->read_buffer); }
            if(priv->delim_buffer){ free(priv->delim_buffer); }
            close(priv->fd);
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

} q2pc_tcp_priv;





static q2pc_tcp_conn_priv* new_conn_priv()
{
    q2pc_tcp_conn_priv* new_priv = calloc(1,sizeof(q2pc_tcp_conn_priv));
    if(!new_priv){
        ch_log_fatal("Malloc failed!\n");
    }

    #define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because
    void* read_buff = calloc(2,BUFF_SIZE);
    if(!read_buff){
        ch_log_fatal("Malloc failed!\n");
    }
    new_priv->read_buffer = read_buff;
    new_priv->read_buffer_size = BUFF_SIZE;

    void* write_buff = (char*)read_buff + BUFF_SIZE;
    new_priv->write_buffer = write_buff;
    new_priv->write_buffer_size = BUFF_SIZE;


    void* working_buff = calloc(1,BUFF_SIZE);
    if(!working_buff){
        ch_log_fatal("Malloc failed!\n");
    }
    new_priv->delim_buffer = working_buff;
    new_priv->delim_buffer_size = BUFF_SIZE;

    return new_priv;

}



//Let a client conect
static int doconnect(struct q2pc_trans_s* this, q2pc_trans_conn* conn)
{
    q2pc_tcp_priv* priv = (q2pc_tcp_priv*)this->priv;
    int fd = -1;
    if(priv->transport.server){
        fd = accept(priv->fd, NULL, NULL);
        if( fd < 0 ){
            ch_log_fatal("TCP accept failed - %s\n",strerror(errno));
        }
    }
    else{
        fd = priv->fd;
    }

    q2pc_tcp_conn_priv* new_priv = new_conn_priv();

    new_priv->fd = fd;
    int flags = 0;
    flags |= O_NONBLOCK;
    if( fcntl(new_priv->fd, F_SETFL, flags) == -1){
        ch_log_fatal("Could not set non-blocking on fd=%i: %s\n",new_priv,strerror(errno));
    }

    conn->priv      = new_priv;
    conn->beg_read  = conn_beg_delimit;
    conn->end_read  = conn_end_delimit;
    conn->beg_write = conn_beg_write;
    conn->end_write = conn_end_write;
    conn->delete    = conn_delete;

    return 0;
}

static void serv_delete(struct q2pc_trans_s* this)
{
    if(this){

        if(this->priv){
            q2pc_tcp_priv* priv = (q2pc_tcp_priv*)this->priv;
            close(priv->fd);
            free(this->priv);
        }

        free(this);
    }

}


#define BUFF_SIZE (4096 * 1024) //A 4MB buffer. Just because
static void init(q2pc_tcp_priv* priv)
{

    ch_log_debug1("Constructing TCP transport\n");

    void* write_all_buff = calloc(1,BUFF_SIZE);
    if(!write_all_buff){
        ch_log_fatal("Malloc for new write all buffer failed!\n");
    }

    priv->fd = socket(AF_INET,SOCK_STREAM,0);
    if (priv->fd < 0 ){
        ch_log_fatal("Could not create TCP socket (%s)\n", strerror(errno));
    }

    int reuse_opt = 1;
    if(setsockopt(priv->fd, SOL_SOCKET, SO_REUSEADDR, &reuse_opt, sizeof(int)) < 0) {
        ch_log_fatal("TCP set option failed: %s\n",strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));

    if(priv->transport.server){
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY; //Listen to any address
        addr.sin_port        = htons(priv->transport.port);

    }
    else{
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = inet_addr(priv->transport.ip);
        addr.sin_port        = htons(priv->transport.port);
    }

    if(priv->transport.server){
        if(bind(priv->fd, (struct sockaddr *)&addr, sizeof(addr)) ){
            uint64_t i = 0;

            //Will wait up to two minutes trying if the address is in use.
            //Helpful for quick restarts of apps as linux keeps some state
            //arround for a while.
            const int64_t seconds_per_try = 5;
            const int64_t seconds_total = 120;
            for(i = 0; i < seconds_total / seconds_per_try && errno == EADDRINUSE; i++){
                ch_log_debug1("%i] %s --> sleeping for %i seconds...\n",i, strerror(errno), seconds_per_try);
                sleep(seconds_per_try);
                bind(priv->fd, (struct sockaddr *)&addr, sizeof(addr));
            }

            if(errno){
                ch_log_fatal("TCP server bind failed: %s\n",strerror(errno));
            }
            else{
                ch_log_debug1("Successfully bound after delay.\n");
            }
        }

        int result = listen(priv->fd, 0);
        if(unlikely( result < 0 )){
            ch_log_fatal("TCP server listen failed: %s\n",strerror(errno));
        }
    }
    else{
        int result = connect(priv->fd,(struct sockaddr *)&addr, sizeof(addr));
        if(unlikely( result < 0 )){
            ch_log_fatal("TCP server connect failed: %s\n",strerror(errno));
        }
    }

    ch_log_debug1("Done constructing TCP transport\n");

}


q2pc_trans* q2pc_tcp_construct(const transport_s* transport)
{
    q2pc_trans* result = (q2pc_trans*)calloc(1,sizeof(q2pc_trans));
    if(!result){
        ch_log_fatal("Could not allocate TCP server structure\n");
    }

    q2pc_tcp_priv* priv = (q2pc_tcp_priv*)calloc(1,sizeof(q2pc_tcp_priv));
    if(!priv){
        ch_log_fatal("Could not allocate TCP server private structure\n");
    }

    result->priv          = priv;
    result->connect       = doconnect;
    result->delete        = serv_delete;
    memcpy(&priv->transport,transport, sizeof(transport_s));
    init(priv);


    return result;
}

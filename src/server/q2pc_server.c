/*
 * q2pc_server.c
 *
 *  Created on: Apr 9, 2014
 *      Author: mgrosvenor
 */

 //#LINKFLAGS=-lpthread

#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "q2pc_server.h"
#include "../transport/q2pc_transport.h"
#include "../errors/errors.h"
#include "../protocol/q2pc_protocol.h"
#include "q2pc_server_worker.h"



//Server wide globals
CH_ARRAY(TRANS_CONN)* cons       = NULL;
CH_ARRAY(i64)* seqs              = NULL;
volatile bool stop_signal        = false;
volatile bool pause_signal       = false;
volatile i64* votes_scoreboard   = NULL;
volatile i64* votes_count        = NULL;
volatile stat_t** stats_mem      = NULL;
volatile bool ack_seen           = false;
i64 msg_size                     = 0;

//File globals
static pthread_t* threads        = NULL;
static i64 real_thread_count     = 0;
static q2pc_trans* trans         = NULL;
static i64 client_count          = 0;
static i64* conn_rtofired_count  = NULL;


static transport_e trans_type    = -1;
static i64 stats_len             = 0;
static i64 total_rtos            = 0;
#define MAX_RTOS (200L * 1000L)

void cleanup()
{
    stop_signal = true;
    __sync_synchronize(); //Full fence

    if(threads){
        for(int i = 0; i < real_thread_count; i++){
            pthread_join(threads[i],NULL);
        }
    }

    if(trans){
        trans->delete(trans);
    }

    int fd = open("/tmp/q2pc_stats", O_WRONLY| O_CREAT | O_TRUNC,  S_IRWXU );
    if(fd < 0){
        ch_log_fatal("Could not open statistics output file error = %s\n", strerror(errno));
    }

    char tmp_line[1024] = {0};

    /*
     *     i64 thread_id;
    i64 client_id;
    i64 c_rtos;
    i64 s_rtos;
    i64 time_start;
    i64 time_end;
     */

    ch_log_info("Total RTOS=%li\n", total_rtos);

    i64 start_us = 0;

    ch_log_info("Writing stats to file...\n");
    for(int i = 0; i < real_thread_count; i++){
        for(int j = 0; j < stats_len / real_thread_count; j++){
            if(j == 0){
                start_us = stats_mem[i][j].time_start;
            }

            int len = snprintf(tmp_line,1024,"%li %li %li %li %li %li %li %li %li\n",
                    stats_mem[i][j].time_start - start_us,
                    stats_mem[i][j].thread_id,
                    stats_mem[i][j].client_id,
                    stats_mem[i][j].c_rtos,
                    stats_mem[i][j].s_rtos,
                    stats_mem[i][j].time_start,
                    stats_mem[i][j].time_end,
                    stats_mem[i][j].time_end -  stats_mem[i][j].time_start,
                    stats_mem[i][j].type);
            write(fd,tmp_line, len);
        }

        free((void*)stats_mem[i]);
    }

    free(stats_mem);
    ch_log_info("Writing stats to file...Done.\n");

    close(fd);

}


//Signal handler to terminate early
void term(int signo)
{
    ch_log_info("Terminating...\n");
    (void)signo;
    cleanup();

    ch_log_info("Terminating... Done.\n");
    exit(0);
}


//Pause/unpause worker threads
void dopause_all(){ pause_signal = true;  __sync_synchronize(); }
void unpause_all(){ pause_signal = false; __sync_synchronize(); }

//Wait for all clients to connect
void do_connectall()
{
    cons = CH_ARRAY_NEW(TRANS_CONN,client_count,NULL);
    seqs = CH_ARRAY_NEW(i64,client_count,NULL);
    if(!cons){ ch_log_fatal("Cannot allocate connections array\n"); }

    i64 connected = 0;
    while(connected < client_count){
        for(int i = 0; i < client_count; i++){

            q2pc_trans_conn* conn = cons->off(cons,i);

            if(!conn->priv){
                //Connections are non-blocking
                if(trans->connect(trans, conn)){
                    continue;
                }
            }


            char* data;
            i64 len;
            if(conn->beg_read(conn,&data, &len)){
                continue;
            }

            if(len < msg_size){
                ch_log_error("Message is smaller than Q2PC message should be. (%li<%li)\n", len, msg_size);
                term(0);
            }

            q2pc_msg* msg = (q2pc_msg*)data;

            switch(msg->type){
                case q2pc_con_msg: connected++; break;
                default:
                    ch_log_error("Unexpected message of type %i\n", msg->type);
                    term(0);
            }

            ch_log_debug3("Connection from %i at index %i\n", msg->src_hostid, i);

            conn->end_read(conn);
        }
    }

}



void server_init(const i64 thread_count, const i64 c_count, const transport_s* transport, i64 stats_l)
{

    //Signal handling for the main thread
    signal(SIGHUP,  term);
    signal(SIGKILL, term);
    signal(SIGTERM, term);
    signal(SIGINT, term);

    client_count = c_count;
    trans_type   = transport->type;
    stats_len    = stats_l;

    //Set up and init the voting scoreboard
    posix_memalign((void*)&votes_scoreboard, sizeof(i64), sizeof(i64) * client_count);
    if(!votes_scoreboard){
        ch_log_fatal("Could not allocate memory for votes scoreboard\n");
    }
    bzero((void*)votes_scoreboard,sizeof(i64) * client_count);

    posix_memalign((void*)&conn_rtofired_count, sizeof(i64), sizeof(i64) * client_count);
    if(!conn_rtofired_count){
        ch_log_fatal("Could not allocate memory for RTO fired counter\n");
    }
    bzero((void*)conn_rtofired_count,sizeof(i64) * client_count);


    //Set up all the connections
    ch_log_info("Waiting for clients to connect...\n\r");
    trans = trans_factory(transport);
    do_connectall();
    ch_log_info("Waiting for clients to connect... Done.\n");


    //Calculate the connection to thread mappings
    i64 cons_per_thread = MAX( (client_count + thread_count -1) / thread_count, 1);
    real_thread_count   = MIN(thread_count, client_count);
    i64 lo = 0;
    i64 hi = lo + cons_per_thread;

    posix_memalign((void*)&votes_count, sizeof(i64), sizeof(i64) * real_thread_count);
    if(!votes_count){
        ch_log_fatal("Could not allocate memory for votes counter\n");
    }
    bzero((void*)votes_count,sizeof(i64) * real_thread_count);


    ch_log_debug1("Allocating stats mem for %li threads with size %i\n", real_thread_count, sizeof(stat_t*));
    stats_mem = calloc(real_thread_count, sizeof(stat_t*));
    if(!stats_mem){
        ch_log_fatal("Could not allocate memory for stats arrays fired counter\n");
    }
    bzero((void*)stats_mem,sizeof(stat_t*) * real_thread_count);


    //Fire up the threads
    threads = (pthread_t*)calloc(real_thread_count, sizeof(pthread_t));
    for(int i = 0; i < real_thread_count; i++){
        ch_log_debug2("Starting thread %i with connections [%li,%li]\n", i, lo, hi -1);

        //Do this to avoid synchronisation errors
        thread_params_t* params = (thread_params_t*)calloc(1,sizeof(thread_params_t));
        if(!params){
            ch_log_fatal("Cannot allocate thread parameters\n");
        }
        params->lo          = lo;
        params->hi          = hi;
        params->count       = client_count;
        params->thread_id   = i;
        params->stats_len   = stats_len / real_thread_count;

        pthread_create(threads + i, NULL, run_thread, (void*)params);

        lo = hi;
        hi = lo + cons_per_thread;
        hi = MIN(cons->size,hi); //Clip so we don't go over the bounds
    }

}

i64 delimit(char* buff, i64 len)
{
    (void)buff;

    if(len >= msg_size){
        return msg_size;
    }

    return 0;
}




static void send_request(q2pc_msg_type_t msg_type)
{
    char* data;
    i64 len;

    //UDP over q-jump uses broadcast on the write, so we only need to send once, and is reliable, so don't have to wait
    if(trans_type == udp_qj){
        q2pc_trans_conn* conn = cons->first;

        if(conn->beg_write(conn,&data,&len)){
            ch_log_fatal("Could not complete broadcast message request\n");
        }

        if(len < msg_size){
            ch_log_fatal("Not enough space to send a Q2PC message. Needed %li, but found %li\n", msg_size, len);
        }

        struct timeval ts_start   = {0};
        gettimeofday(&ts_start, NULL);
        const i64 ts_start_us = ts_start.tv_sec * 1000 * 1000 + ts_start.tv_usec;


        q2pc_msg* msg = (q2pc_msg*)data;
        msg->type       = msg_type;
        msg->src_hostid = ~0LL;
        msg->ts         = ts_start_us;
        msg->s_rto      = 0;
        msg->c_rto      = 0;

        conn->end_write(conn, msg_size);
        return;
    }


    //First, collect all the buffers
    for(int i = 0; i < client_count && !stop_signal; i++){

        q2pc_trans_conn* conn = cons->first + i;

        if(conn->beg_write(conn,&data,&len)){
            ch_log_fatal("Could not complete broadcast message request\n");
        }

        if(len < msg_size){
            ch_log_fatal("Not enough space to send a Q2PC message. Needed %li, but found %li\n", msg_size, len);
        }

        struct timeval ts_start   = {0};
        gettimeofday(&ts_start, NULL);
        const i64 ts_start_us = ts_start.tv_sec * 1000 * 1000 + ts_start.tv_usec;

        q2pc_msg* msg = (q2pc_msg*)data;
        msg->type       = msg_type;
        msg->src_hostid = ~0LL;
        msg->ts         = ts_start_us;
        msg->s_rto      = 0;
        msg->c_rto      = 0;
        ch_log_debug3("Set ts to %li\n", msg->ts) ;

    }

    //Now send them all, and do the RTO timeouts
    int commited = 0;
    bzero(conn_rtofired_count,sizeof(i64) * client_count);

    while(commited < client_count && !stop_signal){
        for(int i = 0; i < client_count && !stop_signal; i++){

            //This is naughty, I'm overloading this, with negative numbers meaning the value is sent
            if(conn_rtofired_count[i] < 0LL){
                ch_log_debug3("Ack'd on client %li. Ignoring for now\n", i);
                continue;
            }

            q2pc_trans_conn* conn = cons->first + i;

            int result = conn->end_write(conn, msg_size);
            switch (result) {
                case Q2PC_RTOFIRED:
                    if(conn_rtofired_count[i] >= MAX_RTOS){ //HACK MAGIC NUMBER!
                        ch_log_error("Connection failed to client %li. Cluster failed after %li RTOS\n", i, MAX_RTOS);
                        term(0);
                    }
                    conn_rtofired_count[i]++;
                    total_rtos++;
                    continue;
                case Q2PC_EAGAIN:
                    continue;
                case Q2PC_ENONE:
                    conn_rtofired_count[i] = -1;
                    commited++;
                    continue;
                case Q2PC_EFIN:
                    ch_log_error("Cannot complete write request, cluster failed\n");
                    term(0);
                default:
                    ch_log_error("Unexpected value (%li) from connection=%li\n", result, i);
                    term(0);
            }
        }
    }
}




typedef enum {  q2pc_request_success, q2pc_request_fail, q2pc_commit_success, q2pc_commit_fail, q2pc_cluster_fail } q2pc_commit_status_t;

void wait_for_votes(i64 timeout_us)
{
    struct timeval ts_start = {0};
    struct timeval ts_now   = {0};
    i64 ts_start_us         = 0;
    i64 ts_now_us           = 0;

    gettimeofday(&ts_start, NULL);
    ts_start_us = ts_start.tv_sec * 1000 * 1000 + ts_start.tv_usec;


    //Wait to either timeout or for all votes to be counted
    ch_log_debug2("Q2PC Server: [M] Waiting for votes\n");
    while(1){
        if(timeout_us >= 0){
             gettimeofday(&ts_now, NULL);
             ts_now_us = ts_now.tv_sec * 1000 * 1000 + ts_now.tv_usec;
             if(ts_now_us > ts_start_us + timeout_us){
                 ch_log_warn("Timed out waiting for client response(s)\n");
                 break;
             }
        }

        i64 total_votes = 0;
        for(int i = 0; i < real_thread_count; i++){
            total_votes+= votes_count[i];
        }
        if(total_votes >= client_count){
            ch_log_debug2("Q2PC Server: [M] Done, collected %li votes\n", total_votes);
            break;
        }
    }

}


q2pc_commit_status_t do_phase1(i64 cluster_timeout_us)
{

    q2pc_commit_status_t result = q2pc_request_success;

    //Init the scoreboard
    for(int i = 0; i < client_count; i++){
        __builtin_prefetch((char*)votes_scoreboard + i + 1);
        votes_scoreboard[i] = q2pc_lost_msg;
    }

    //send out a broadcast message to all servers
    ch_log_debug2("Q2PC Server: [M]--> request\n");
    send_request(q2pc_request_msg);

    //wait for all the responses
    wait_for_votes(cluster_timeout_us);

    //Stop all the receiver threads
    dopause_all();
    for(int i = 0; i < client_count && !stop_signal; i++){
        __builtin_prefetch((char*)votes_scoreboard + i + 1);
        switch(votes_scoreboard[i]){
            case q2pc_vote_yes_msg:
                ch_log_debug1("client %li voted yes.\n",i);
                continue;

            case q2pc_vote_no_msg:
                ch_log_debug1("client %li voted no.\n",i);
                result = q2pc_request_fail;
                break;

            case q2pc_lost_msg:
                ch_log_warn("Q2PC: phase 1 - client %li message lost, cluster failed\n",i);
                result = q2pc_cluster_fail;
                break;

            default:
                ch_log_debug1("Q2PC: Server [M] phase 1 - client %li sent an unexpected message type %i\n",i,votes_scoreboard[i]);
                ch_log_error("Protocol violation\n");
                term(0);
        }
    }

    for(int i = 0; i < real_thread_count; i++){
        votes_count[i] = 0;
    }

    unpause_all();

    return result;
}


q2pc_commit_status_t do_phase2(q2pc_commit_status_t phase1_status, i64 cluster_timeout_us)
{
    //Init the scoreboard
    for(int i = 0; i < client_count; i++){
        __builtin_prefetch((char*)votes_scoreboard + i + 1);
        votes_scoreboard[i] = q2pc_lost_msg;
    }

    switch(phase1_status){
        case q2pc_request_success:
            ch_log_debug2("Q2PC Server: [M]--> commit\n");
            send_request(q2pc_commit_msg);
            break;
        case q2pc_request_fail:
            ch_log_debug2("Q2PC Server: [M]--> cancel\n");
            send_request(q2pc_cancel_msg);
            break;
        case q2pc_cluster_fail:
            return q2pc_cluster_fail;
        default:
            ch_log_error("Internal error: unexpected result from phase 1\n");
            term(0);
    }

    //wait for all the responses
    wait_for_votes(cluster_timeout_us);

    //Stop all the receiver threads
    dopause_all();
    q2pc_commit_status_t result = q2pc_commit_success;
    for(int i = 0; i < client_count && !stop_signal; i++){
        __builtin_prefetch((char*)votes_scoreboard + i + 1);

        switch(votes_scoreboard[i]){
            case q2pc_ack_msg:
                continue;

            case q2pc_lost_msg:
                ch_log_warn("Q2PC: Server [M] phase 2 - client %li message lost, cluster failed\n",i);
                result = q2pc_cluster_fail;
                break;
            default:
                ch_log_debug1("Q2PC: Server [M] phase 2 - client %li sent an unexpected message type %i\n",i,votes_scoreboard[i]);
                result = q2pc_cluster_fail;
        }
    }

    for(int i = 0; i < real_thread_count; i++){
        votes_count[i] = 0;
    }

    unpause_all();

    if(result == q2pc_cluster_fail){
        return q2pc_cluster_fail;
    }

    switch(phase1_status){
        case q2pc_request_success:  return q2pc_commit_success;
        case q2pc_request_fail:     return q2pc_commit_fail;
        default:
            ch_log_error("Internal error: unexpected result from phase 1\n");
            term(0);
    }

    //Unreachable
    return -1;

}

void run_server(const i64 thread_count, const i64 client_count,  const transport_s* transport, i64 wait_time, i64 report_int, i64 stats_len, i64 msize)
{

    //Statistics keeping
    struct timeval ts_start = {0};
    struct timeval ts_now   = {0};
    i64 ts_start_us         = 0;
    i64 ts_now_us           = 0;
    msg_size                = MAX((i64)sizeof(q2pc_msg),msize);
    ch_log_info("Using message size of %li\n", msg_size);

    //Set up all the threads, scoreboard, transport connections etc.
    server_init(thread_count, client_count, transport, stats_len);

    gettimeofday(&ts_start, NULL);
    ts_start_us = ts_start.tv_sec * 1000 * 1000 + ts_start.tv_usec;

    ch_log_info("Running...\n");
    for(i64 requests = 0; !stop_signal; requests++){

        if(requests && (requests % report_int == 0) ){
            gettimeofday(&ts_now, NULL);
            ts_now_us = ts_now.tv_sec * 1000 * 1000 + ts_now.tv_usec;

            const i64 time_taken_us = ts_now_us - ts_start_us;
            double reqs_per_sec = (double)report_int / (double)(time_taken_us) * 1000 * 1000;

            ch_log_info("Running at %0.2lf req/s (%li)\n", reqs_per_sec, time_taken_us);

            gettimeofday(&ts_start, NULL);
            ts_start_us = ts_start.tv_sec * 1000 * 1000 + ts_start.tv_usec;
        }


        q2pc_commit_status_t status;
        status = do_phase1(wait_time);
        status = do_phase2(status, wait_time);

        switch(status){
            case q2pc_cluster_fail:     ch_log_error("Cluster failed\n"); term(0);break;
            case q2pc_commit_success:   ch_log_debug1("Commit success!\n"); break;
            case q2pc_commit_fail:      ch_log_debug1("Commit fail!\n"); break;
            default:
                ch_log_error("Internal error: unexpected result from phase 2\n");
                term(0);
        }
    }

    term(0);

}


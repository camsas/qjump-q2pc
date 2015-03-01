/*

* q2pc_server_worker.c
 *
 *  Created on: Apr 17, 2014
 *      Author: mgrosvenor
 */

//#LINKFLAGS=-lpthread

#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>

#include "q2pc_server.h"
#include "../transport/q2pc_transport.h"
#include "../errors/errors.h"
#include "../protocol/q2pc_protocol.h"
#include "q2pc_server_worker.h"

//Globals that matter
extern CH_ARRAY(TRANS_CONN)* cons;
extern CH_ARRAY(i64)* seqs;
extern volatile bool stop_signal;
extern volatile bool pause_signal;
//static pthread_t* threads               = NULL;
//static i64 real_thread_count            = 0;
extern volatile i64* votes_scoreboard ;
extern volatile i64* votes_count;
extern stat_t** stats_mem;
//static q2pc_trans* trans                = NULL;
//static volatile i64 seq_no              = 0;


#define BARRIER()  __asm__ volatile("" ::: "memory")

void* run_thread( void* p)
{
    thread_params_t* params = (thread_params_t*)p;
    i64 lo          = params->lo;
    i64 hi          = params->hi;
    i64 count       = params->count;
    i64 thread_id   = params->thread_id;
    i64 stats_len   = params->stats_len;
    free(params);

    i64 stats_idx = 0;

    //ch_log_debug1("Allocating ")
    stats_mem[thread_id] = calloc(stats_len, sizeof(stat_t));
    if(!stats_mem[thread_id]){
        ch_log_fatal("Could not allocate %liB of memory for statistics counter\n", sizeof(stat_t) * stats_len);
    }


    ch_log_debug3("Running worker thread\n");
    while(!stop_signal){

        //Busy loop if we're told to stop processing for a moment
        if(pause_signal){
            votes_count[thread_id] = 0;
            __asm__("pause");
            continue;
        }

        //Otherwise, busy loop looking for data
        for(int i = lo; i < hi; i++){
            q2pc_trans_conn* con = cons->off(cons,i);
            char* data = NULL;
            i64 len = 0;
            i64 result = con->beg_read(con,&data, &len);
            if(result){
                if(result == Q2PC_EAGAIN){
                    continue;
                }

                if(result == Q2PC_EFIN){
                    stop_signal = 1;
                    BARRIER();
                    ch_log_warn("Cannot read any more data from connection %li on thread %li. Stream has finished\n", i, thread_id);
                    usleep(1000); //A a bit for the signal to propagate
                    break;

                }

            }

            q2pc_msg* msg = (q2pc_msg*)data;
            //Bounds check the answer

            if(msg->src_hostid < 1 || msg->src_hostid > count){
                ch_log_warn("Client ID (%li) is out of the expected range [%i,%i]. Ignoring vote\n", msg->src_hostid, 1, count);
                //con->end_read(con);
                continue;
            }

            votes_scoreboard[msg->src_hostid - 1] = msg->type;
            switch(msg->type){
                case q2pc_vote_yes_msg: ch_log_debug2("Q2PC Server: [%i]<-- vote yes from (%li)\n", thread_id, msg->src_hostid); break;
                case q2pc_vote_no_msg:  ch_log_debug2("Q2PC Server: [%i]<-- vote no  from (%li)\n", thread_id, msg->src_hostid); break;
                case q2pc_ack_msg:      ch_log_debug2("Q2PC Server: [%i]<-- ack      from (%li)\n", thread_id, msg->src_hostid); break;
                default:
                    ch_log_warn("Q2PC Server: [%i] <-- Unknown message (%i)   from (%li)\n",thread_id, msg->type, msg->src_hostid );
            }
            con->end_read(con);
            BARRIER(); //Make sure there is no memory reordering here

            votes_count[thread_id]++;
            BARRIER();

            struct timeval ts_end   = {0};
            gettimeofday(&ts_end, NULL);
            const i64 ts_end_us = ts_end.tv_sec * 1000 * 1000 + ts_end.tv_usec;

            ch_log_debug3("Got ts with %li\n", msg->ts) ;

            stats_mem[thread_id][stats_idx].time_end   = ts_end_us;
            stats_mem[thread_id][stats_idx].thread_id  = thread_id;
            stats_mem[thread_id][stats_idx].time_start = msg->ts;
            stats_mem[thread_id][stats_idx].client_id  = msg->src_hostid;
            stats_mem[thread_id][stats_idx].c_rtos     = msg->c_rto;
            stats_mem[thread_id][stats_idx].s_rtos     = msg->s_rto;
            stats_mem[thread_id][stats_idx].type       = msg->type;


            stats_idx++;
            if(stats_idx >= stats_len){
                stop_signal = 1;
                BARRIER();
                ch_log_warn("Run out of stats memory in thread %li Exiting\n", thread_id);
                usleep(1000); //A a bit for the signal to propagate
                break;
            }

            ch_log_debug2("Q2PC Server: [%li] Vote count=%li\n", thread_id,votes_count[thread_id]);

        }
    }



    ch_log_debug3("Cleaning up connections...\n");
    //We're done with the connections now, clean them up
    for(int i = lo; i < hi; i++){
        q2pc_trans_conn* con = cons->off(cons,i);
        con->delete(con);
    }

    ch_log_debug3("Exiting worker thread\n");
    return NULL;
}


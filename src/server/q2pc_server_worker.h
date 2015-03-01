/*
 * q2pc_server_worker.h
 *
 *  Created on: Apr 17, 2014
 *      Author: mgrosvenor
 */

#ifndef Q2PC_SERVER_WORKER_H_
#define Q2PC_SERVER_WORKER_H_


typedef struct{
    i64 lo;
    i64 hi;
    i64 count;
    i64 thread_id;
    i64 stats_len;
} thread_params_t;

typedef struct{
    i64 thread_id;
    i64 client_id;
    i64 c_rtos;
    i64 s_rtos;
    i64 time_start;
    i64 time_end;
    i64 type;
} stat_t;


void* run_thread( void* p);


#endif /* Q2PC_SERVER_WORKER_H_ */

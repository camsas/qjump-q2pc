#include <stdio.h>
#include "../deps/chaste/chaste.h"
#include "../deps/chaste/options/options.h"

#include "server/q2pc_server.h"
#include "client/q2pc_client.h"
#include "transport/q2pc_transport.h"

USE_CH_LOGGER(CH_LOG_LVL_INFO,true,ch_log_tostderr,NULL);
USE_CH_OPTIONS;

static struct {
	//Server Options
	i64 server;
	i64 threads;

	//Client Options
	char* client;
	i64 client_id;

	//Transports
	bool trans_tcp_ln;
	bool trans_udp_ln;
	bool trans_rdp_ln;
	bool trans_udp_qj;

	//Transport options
    char* bcast;
	i64 port;
	i64 qjump_epoch;
	i64 qjump_psize;
	char* iface;
	i64 msize;

	//Logging options
	bool log_no_colour;
	bool log_stdout;
	bool log_stderr;
	bool log_syslog;
	char* log_filename;
	i64 log_verbosity;

	//General
	i64 waittime;
	i64 rto_us;
	i64 report_int;
	i64 stats_len;

} options;


int main(int argc, char** argv)
{
	//Server options
    ch_opt_addii(CH_OPTION_OPTIONAL,'s',"server","Put q2pc in server mode, specify the number of clients", &options.server, 0);
    ch_opt_addii(CH_OPTION_OPTIONAL,'T',"threads","The number of threads to use", &options.threads, 1);

    //Client options
    ch_opt_addsi(CH_OPTION_OPTIONAL,'c',"client","Put q2pc in client mode, specify server address in x.x.x.x format", &options.client, NULL);
    ch_opt_addii(CH_OPTION_OPTIONAL,'C',"id","The client ID to use for this client (must be >0)", &options.client_id, -1);

    //Transports
    ch_opt_addbi(CH_OPTION_FLAG,    'u',"udp-ln","Use Linux based UDP transport [default]", &options.trans_udp_ln, false);
    ch_opt_addbi(CH_OPTION_FLAG,    't',"tcp-ln","Use Linux based TCP transport", &options.trans_tcp_ln, false);
    ch_opt_addbi(CH_OPTION_FLAG,    'r',"rdp-ln","Use Linux based UDP transport with reliability", &options.trans_rdp_ln, false);
    ch_opt_addbi(CH_OPTION_FLAG,    'q',"udp-qj","Use NetMap based UDP transport over Q-Jump", &options.trans_udp_qj, false);

    //Qjump Transport options
    ch_opt_addii(CH_OPTION_OPTIONAL,'p',"port","Port to use for all transports", &options.port, 7331);
    ch_opt_addsi(CH_OPTION_OPTIONAL,'B',"broadcast","The broadcast IP address to use in UDP mode ini x.x.x.x format", &options.bcast, "127.0.0.0");
    ch_opt_addsi(CH_OPTION_OPTIONAL,'i',"iface","The interface name to use", &options.iface, "eth4");
    ch_opt_addii(CH_OPTION_OPTIONAL,'m',"message-size","Size of the messages to use", &options.msize, 128);

    //Q2PC Logging
    ch_opt_addbi(CH_OPTION_FLAG,     'n', "no-colour",  "Turn off colour log output",     &options.log_no_colour, false);
    ch_opt_addbi(CH_OPTION_FLAG,     '0', "log-stdout", "Log to standard out", 	 	&options.log_stdout, 	false);
    ch_opt_addbi(CH_OPTION_FLAG,     '1', "log-stderr", "Log to standard error [default]", 	&options.log_stderr, 	false);
    ch_opt_addsi(CH_OPTION_OPTIONAL, 'F', "log-file",   "Log to the file supplied",	&options.log_filename, 	NULL);
    ch_opt_addii(CH_OPTION_OPTIONAL, 'v', "log-level",  "Log level verbosity (0 = lowest, 6 = highest)",  &options.log_verbosity, CH_LOG_LVL_INFO);

    ch_opt_addii(CH_OPTION_OPTIONAL, 'w',"wait","How long to wait for client/server delay (us)", &options.waittime, 2000 * 1000);
    ch_opt_addii(CH_OPTION_OPTIONAL, 'o',"rto", "How long to wait before retransmitting a request (us)", &options.rto_us, 200 * 1000);
    ch_opt_addii(CH_OPTION_OPTIONAL, 'R',"report-int", "reporting interval for statistics", &options.report_int, 100);
    ch_opt_addii(CH_OPTION_OPTIONAL, 'S',"stats-len", "length of stats to keep", &options.stats_len, 1000);
    //Parse it all up
    ch_opt_parse(argc,argv);


    //Configure logging
    ch_log_settings.filename    = options.log_filename;
    ch_log_settings.use_color   = !options.log_no_colour;
    ch_log_settings.log_level   = MAX(0, MIN(options.log_verbosity, CH_LOG_LVL_DEBUG3)); //Put a bound on it

    i64 log_opt_count = 0;
    log_opt_count += options.log_stderr ?  1 : 0;
    log_opt_count += options.log_stdout ?  1 : 0;
    log_opt_count += options.log_syslog ?  1 : 0;
    log_opt_count += options.log_filename? 1 : 0;

    //Too many options selected
    if(log_opt_count > 1){
        ch_log_fatal("Q2PC: Can only log to one format at a time, you've selected the following [%s%s%s%s]\n ",
                options.log_stderr ? "std-err " : "",
                options.log_stdout ? "std-out " : "",
                options.log_syslog ? "system log " : "",
                options.log_filename ? "file out " : ""
        );
    }

    //No option selected, use the default
    if(log_opt_count == 0){
        options.log_stdout = true;
    }

    //Configure the options as desired
    if(options.log_stderr){
        ch_log_settings.output_mode = ch_log_tostderr;
    } else if(options.log_stdout){
        ch_log_settings.output_mode = ch_log_tostdout;
    } else if(options.log_syslog){
        ch_log_settings.output_mode = ch_log_tosyslog;
    } else if (options.log_filename){
        ch_log_settings.output_mode = ch_log_tofile;
    }

    i64 transport_opt_count = 0;
    transport_opt_count += options.trans_udp_ln ? 1 : 0;
    transport_opt_count += options.trans_tcp_ln ? 1 : 0;
    transport_opt_count += options.trans_rdp_ln ? 1 : 0;
    transport_opt_count += options.trans_udp_qj ? 1 : 0;

    //Make sure only 1 choice has been made
    if(transport_opt_count > 1){
        ch_log_fatal("Q2PC: Can only use one transport at a time, you've selected the following [%s%s%s%s%s ]\n ",
                options.trans_udp_ln ? "udp-ln " : "",
                options.trans_tcp_ln ? "tcp-ln " : "",
                options.trans_tcp_ln ? "rdp-ln " : "",
                options.trans_udp_qj ? "udp-qj " : ""
        );
    }

    //Set up the default
    if(transport_opt_count == 0){
        options.trans_udp_ln = 1;
    }

    //Finally figure out he actual one that we want
    transport_s transport = {0};
    transport.type          = options.trans_udp_ln ? udp_ln : transport.type;
    transport.type          = options.trans_tcp_ln ? tcp_ln : transport.type;
    transport.type          = options.trans_udp_qj ? udp_qj : transport.type;
    transport.type          = options.trans_rdp_ln ? rdp_ln : transport.type;
    transport.qjump_epoch   = options.qjump_epoch;
    transport.qjump_limit   = options.qjump_psize;
    transport.port          = options.port;
    transport.ip            = options.client;
    transport.server        = options.server;
    transport.client_count  = options.server;
    transport.client_id     = options.client_id;
    transport.bcast         = options.bcast;
    transport.iface         = options.iface;
    transport.rto_us        = options.rto_us;
    transport.msize         = options.msize;


    //Configure application options
    if(options.client && options.server ){
        ch_log_fatal("Q2PC: Configuration error, must be in client or server mode, not both.\n");
    }
    if(!options.client && !options.server ){
        ch_log_fatal("Q2PC: Configuration error, in server mode, you must specify at least 1 client.\n");
    }


    if(options.client && options.client_id < 0){
        ch_log_fatal("Q2PC: Configuration error, in client mode, you must specify a client id >0.\n");
    }


    /********************************************************/
    //real work begins here:
    /********************************************************/
    if(options.client){
        run_client(&transport, options.client_id, options.waittime, options.msize);
    }
    else{
        run_server(options.threads, options.server,&transport, options.waittime, options.report_int, options.stats_len, options.msize);
    }

    return 0;
}


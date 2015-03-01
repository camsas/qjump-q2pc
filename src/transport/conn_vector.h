/*
 * conn_vector.h
 *
 *  Created on: Apr 9, 2014
 *      Author: mgrosvenor
 */

#ifndef CONN_VECTOR_H_
#define CONN_VECTOR_H_

#include "q2pc_transport.h"
#include "../../deps/chaste/data_structs/vector/vector.h"
#include "../../deps/chaste/data_structs/vector/vector_typed_declare_template.h"

struct q2pc_trans_conn_s; //Forward declaration, look in q2pc_transport.h for definition
declare_ch_vector(TRANS_CONN,struct q2pc_trans_conn_s)


#endif /* CONN_VECTOR_H_ */

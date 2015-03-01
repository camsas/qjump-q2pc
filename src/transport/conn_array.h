/*
 * conn_vector.h
 *
 *  Created on: Apr 9, 2014
 *      Author: mgrosvenor
 */

#ifndef CONN_ARRAY_H_
#define CONN_ARRAY_H_

#include "q2pc_transport.h"
#include "../../deps/chaste/data_structs/array/array.h"
#include "../../deps/chaste/data_structs/array/array_typed_declare_template.h"

struct q2pc_trans_conn_s; //Forward declaration, look in q2pc_transport.h for definition
declare_array(TRANS_CONN,struct q2pc_trans_conn_s)


#endif /* CONN_VECTOR_H_ */

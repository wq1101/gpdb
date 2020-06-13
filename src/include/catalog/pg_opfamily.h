/*-------------------------------------------------------------------------
 *
 * pg_opfamily.h
 *	  definition of the system "opfamily" relation (pg_opfamily)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_opfamily.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPFAMILY_H
#define PG_OPFAMILY_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_opfamily definition. cpp turns this into
 *		typedef struct FormData_pg_opfamily
 * ----------------
 */
#define OperatorFamilyRelationId  2753

CATALOG(pg_opfamily,2753)
{
	Oid			opfmethod;		/* index access method opfamily is for */
	NameData	opfname;		/* name of this opfamily */
	Oid			opfnamespace;	/* namespace of this opfamily */
	Oid			opfowner;		/* opfamily owner */
} FormData_pg_opfamily;

/* GPDB added foreign key definitions for gpcheckcat. */
FOREIGN_KEY(opfmethod REFERENCES pg_am(oid));
FOREIGN_KEY(opfnamespace REFERENCES pg_namespace(oid));
FOREIGN_KEY(opfowner REFERENCES pg_authid(oid));

/* ----------------
 *		Form_pg_opfamily corresponds to a pointer to a tuple with
 *		the format of pg_opfamily relation.
 * ----------------
 */
typedef FormData_pg_opfamily *Form_pg_opfamily;

/* ----------------
 *		compiler constants for pg_opfamily
 * ----------------
 */
#define Natts_pg_opfamily				4
#define Anum_pg_opfamily_opfmethod		1
#define Anum_pg_opfamily_opfname		2
#define Anum_pg_opfamily_opfnamespace	3
#define Anum_pg_opfamily_opfowner		4

/* ----------------
 *		initial contents of pg_opfamily
 * ----------------
 */

DATA(insert OID =  421 (	403		abstime_ops		PGNSP PGUID ));
DATA(insert OID =  397 (	403		array_ops		PGNSP PGUID ));
DATA(insert OID =  627 (	405		array_ops		PGNSP PGUID ));
DATA(insert OID =  423 (	403		bit_ops			PGNSP PGUID ));
DATA(insert OID =  424 (	403		bool_ops		PGNSP PGUID ));
#define BOOL_BTREE_FAM_OID 424
DATA(insert OID =  426 (	403		bpchar_ops		PGNSP PGUID ));
#define BPCHAR_BTREE_FAM_OID 426
DATA(insert OID =  427 (	405		bpchar_ops		PGNSP PGUID ));
DATA(insert OID =  428 (	403		bytea_ops		PGNSP PGUID ));
#define BYTEA_BTREE_FAM_OID 428
DATA(insert OID =  429 (	403		char_ops		PGNSP PGUID ));
DATA(insert OID =  431 (	405		char_ops		PGNSP PGUID ));
DATA(insert OID =  434 (	403		datetime_ops	PGNSP PGUID ));
DATA(insert OID =  435 (	405		date_ops		PGNSP PGUID ));
DATA(insert OID = 1970 (	403		float_ops		PGNSP PGUID ));
DATA(insert OID = 1971 (	405		float_ops		PGNSP PGUID ));
DATA(insert OID = 1974 (	403		network_ops		PGNSP PGUID ));
#define NETWORK_BTREE_FAM_OID 1974
DATA(insert OID = 1975 (	405		network_ops		PGNSP PGUID ));
DATA(insert OID = 3550 (	783		network_ops		PGNSP PGUID ));
DATA(insert OID = 1976 (	403		integer_ops		PGNSP PGUID ));
#define INTEGER_BTREE_FAM_OID 1976
DATA(insert OID = 1977 (	405		integer_ops		PGNSP PGUID ));
DATA(insert OID = 1982 (	403		interval_ops	PGNSP PGUID ));
DATA(insert OID = 1983 (	405		interval_ops	PGNSP PGUID ));
DATA(insert OID = 1984 (	403		macaddr_ops		PGNSP PGUID ));
DATA(insert OID = 1985 (	405		macaddr_ops		PGNSP PGUID ));
DATA(insert OID = 1986 (	403		name_ops		PGNSP PGUID ));
#define NAME_BTREE_FAM_OID 1986
DATA(insert OID = 1987 (	405		name_ops		PGNSP PGUID ));
DATA(insert OID = 1988 (	403		numeric_ops		PGNSP PGUID ));
DATA(insert OID = 1998 (	405		numeric_ops		PGNSP PGUID ));
DATA(insert OID = 1989 (	403		oid_ops			PGNSP PGUID ));
#define OID_BTREE_FAM_OID 1989
DATA(insert OID = 1990 (	405		oid_ops			PGNSP PGUID ));
DATA(insert OID = 1991 (	403		oidvector_ops	PGNSP PGUID ));
DATA(insert OID = 1992 (	405		oidvector_ops	PGNSP PGUID ));
DATA(insert OID = 2994 (	403		record_ops		PGNSP PGUID ));
DATA(insert OID = 3194 (	403		record_image_ops	PGNSP PGUID ));
DATA(insert OID = 1994 (	403		text_ops		PGNSP PGUID ));
#define TEXT_BTREE_FAM_OID 1994
DATA(insert OID = 1995 (	405		text_ops		PGNSP PGUID ));
DATA(insert OID = 1996 (	403		time_ops		PGNSP PGUID ));
DATA(insert OID = 1997 (	405		time_ops		PGNSP PGUID ));
DATA(insert OID = 1999 (	405		timestamptz_ops PGNSP PGUID ));
DATA(insert OID = 2000 (	403		timetz_ops		PGNSP PGUID ));
DATA(insert OID = 2001 (	405		timetz_ops		PGNSP PGUID ));
DATA(insert OID = 2002 (	403		varbit_ops		PGNSP PGUID ));
DATA(insert OID = 2040 (	405		timestamp_ops	PGNSP PGUID ));
DATA(insert OID = 2095 (	403		text_pattern_ops	PGNSP PGUID ));
#define TEXT_PATTERN_BTREE_FAM_OID 2095
DATA(insert OID = 2097 (	403		bpchar_pattern_ops	PGNSP PGUID ));
#define BPCHAR_PATTERN_BTREE_FAM_OID 2097
DATA(insert OID = 2099 (	403		money_ops		PGNSP PGUID ));
DATA(insert OID = 2222 (	405		bool_ops		PGNSP PGUID ));
#define BOOL_HASH_FAM_OID 2222
DATA(insert OID = 2223 (	405		bytea_ops		PGNSP PGUID ));
DATA(insert OID = 2224 (	405		int2vector_ops	PGNSP PGUID ));
DATA(insert OID = 2789 (	403		tid_ops			PGNSP PGUID ));
DATA(insert OID = 2225 (	405		xid_ops			PGNSP PGUID ));
DATA(insert OID = 2226 (	405		cid_ops			PGNSP PGUID ));
DATA(insert OID = 2227 (	405		abstime_ops		PGNSP PGUID ));
DATA(insert OID = 2228 (	405		reltime_ops		PGNSP PGUID ));
DATA(insert OID = 2229 (	405		text_pattern_ops	PGNSP PGUID ));
DATA(insert OID = 2231 (	405		bpchar_pattern_ops	PGNSP PGUID ));
DATA(insert OID = 2233 (	403		reltime_ops		PGNSP PGUID ));
DATA(insert OID = 2234 (	403		tinterval_ops	PGNSP PGUID ));
DATA(insert OID = 2235 (	405		aclitem_ops		PGNSP PGUID ));
DATA(insert OID = 2593 (	783		box_ops			PGNSP PGUID ));
DATA(insert OID = 2594 (	783		poly_ops		PGNSP PGUID ));
DATA(insert OID = 2595 (	783		circle_ops		PGNSP PGUID ));
DATA(insert OID = 1029 (	783		point_ops		PGNSP PGUID ));
DATA(insert OID = 2745 (	2742	array_ops		PGNSP PGUID ));
DATA(insert OID = 2968 (	403		uuid_ops		PGNSP PGUID ));
DATA(insert OID = 2969 (	405		uuid_ops		PGNSP PGUID ));
DATA(insert OID = 3253 (	403		pg_lsn_ops		PGNSP PGUID ));
DATA(insert OID = 3254 (	405		pg_lsn_ops		PGNSP PGUID ));
DATA(insert OID = 3522 (	403		enum_ops		PGNSP PGUID ));
DATA(insert OID = 3523 (	405		enum_ops		PGNSP PGUID ));
DATA(insert OID = 3626 (	403		tsvector_ops	PGNSP PGUID ));
DATA(insert OID = 3655 (	783		tsvector_ops	PGNSP PGUID ));
DATA(insert OID = 3659 (	2742	tsvector_ops	PGNSP PGUID ));
DATA(insert OID = 3683 (	403		tsquery_ops		PGNSP PGUID ));
DATA(insert OID = 3702 (	783		tsquery_ops		PGNSP PGUID ));
DATA(insert OID = 3901 (	403		range_ops		PGNSP PGUID ));
DATA(insert OID = 3903 (	405		range_ops		PGNSP PGUID ));
DATA(insert OID = 3919 (	783		range_ops		PGNSP PGUID ));
DATA(insert OID = 3474 (	4000	range_ops		PGNSP PGUID ));
DATA(insert OID = 4015 (	4000	quad_point_ops	PGNSP PGUID ));
DATA(insert OID = 4016 (	4000	kd_point_ops	PGNSP PGUID ));
DATA(insert OID = 4017 (	4000	text_ops		PGNSP PGUID ));
#define TEXT_SPGIST_FAM_OID 4017
DATA(insert OID = 4033 (	403		jsonb_ops		PGNSP PGUID ));
DATA(insert OID = 4034 (	405		jsonb_ops		PGNSP PGUID ));
DATA(insert OID = 4035 (	783		jsonb_ops		PGNSP PGUID ));
DATA(insert OID = 4036 (	2742	jsonb_ops		PGNSP PGUID ));
DATA(insert OID = 4037 (	2742	jsonb_path_ops	PGNSP PGUID ));

DATA(insert OID = 4054 (	3580	integer_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4055 (	3580	numeric_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4056 (	3580	text_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4058 (	3580	timetz_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4059 (	3580	datetime_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4062 (	3580	char_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4064 (	3580	bytea_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4065 (	3580	name_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4068 (	3580	oid_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4069 (	3580	tid_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4070 (	3580	float_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4072 (	3580	abstime_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4073 (	3580	reltime_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4074 (	3580	macaddr_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4075 (	3580	network_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4102 (	3580	network_inclusion_ops	PGNSP PGUID ));
DATA(insert OID = 4076 (	3580	bpchar_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4077 (	3580	time_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4078 (	3580	interval_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4079 (	3580	bit_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4080 (	3580	varbit_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4081 (	3580	uuid_minmax_ops			PGNSP PGUID ));
DATA(insert OID = 4103 (	3580	range_inclusion_ops		PGNSP PGUID ));
DATA(insert OID = 4082 (	3580	pg_lsn_minmax_ops		PGNSP PGUID ));
DATA(insert OID = 4104 (	3580	box_inclusion_ops		PGNSP PGUID ));
DATA(insert OID = 5000 (	4000	box_ops		PGNSP PGUID ));

/* Complex Number type */
DATA(insert OID = 6221 (	403		complex_ops		PGNSP PGUID ));
DATA(insert OID = 6224 (	405		complex_ops		PGNSP PGUID ));

/*
 * hash support for a few built-in datatypes that are missing it in upstream.
 */
DATA(insert OID = 7077 (	405		tid_ops		PGNSP PGUID ));
DATA(insert OID = 7078 (	405		bit_ops		PGNSP PGUID ));
DATA(insert OID = 7079 (	405		varbit_ops	PGNSP PGUID ));

/* Hash opfamilies to represent the legacy "cdbhash" function */
/* int2, int4, int8 */
DATA(insert OID = 7100 (	405		cdbhash_integer_ops	PGNSP PGUID ));
DATA(insert OID = 7101 (	405		cdbhash_float4_ops	PGNSP PGUID ));
DATA(insert OID = 7102 (	405		cdbhash_float8_ops	PGNSP PGUID ));
DATA(insert OID = 7103 (	405		cdbhash_numeric_ops	PGNSP PGUID ));
DATA(insert OID = 7104 (	405		cdbhash_char_ops	PGNSP PGUID ));
/* text is also for varchar */
DATA(insert OID = 7105 (	405		cdbhash_text_ops	PGNSP PGUID ));
/*
 * In the legacy hash functions, the hashing of 'text' and 'bpchar' are
 * actually compatible. But there are no cross-datatype operators between
 * them, we rely on casts. Better to put them in different operator families,
 * to avoid confusion.
 */
DATA(insert OID = 7106 (	405		cdbhash_bpchar_ops	PGNSP PGUID ));
DATA(insert OID = 7107 (	405		cdbhash_bytea_ops	PGNSP PGUID ));
DATA(insert OID = 7108 (	405		cdbhash_name_ops	PGNSP PGUID ));
DATA(insert OID = 7109 (	405		cdbhash_oid_ops		PGNSP PGUID ));
DATA(insert OID = 7110 (	405		cdbhash_tid_ops		PGNSP PGUID ));
DATA(insert OID = 7111 (	405		cdbhash_timestamp_ops	PGNSP PGUID ));
DATA(insert OID = 7112 (	405		cdbhash_timestamptz_ops	PGNSP PGUID ));
DATA(insert OID = 7113 (	405		cdbhash_date_ops	PGNSP PGUID ));
DATA(insert OID = 7114 (	405		cdbhash_time_ops	PGNSP PGUID ));
DATA(insert OID = 7115 (	405		cdbhash_timetz_ops	PGNSP PGUID ));
DATA(insert OID = 7116 (	405		cdbhash_interval_ops	PGNSP PGUID ));
DATA(insert OID = 7117 (	405		cdbhash_abstime_ops	PGNSP PGUID ));
DATA(insert OID = 7118 (	405		cdbhash_reltime_ops	PGNSP PGUID ));
DATA(insert OID = 7119 (	405		cdbhash_tinterval_ops	PGNSP PGUID ));
DATA(insert OID = 7120 (	405		cdbhash_inet_ops	PGNSP PGUID ));
DATA(insert OID = 7121 (	405		cdbhash_macaddr_ops	PGNSP PGUID ));
DATA(insert OID = 7122 (	405		cdbhash_bit_ops		PGNSP PGUID ));
DATA(insert OID = 7123 (	405		cdbhash_varbit_ops	PGNSP PGUID ));
DATA(insert OID = 7124 (	405		cdbhash_bool_ops	PGNSP PGUID ));
DATA(insert OID = 7125 (	405		cdbhash_array_ops	PGNSP PGUID ));
DATA(insert OID = 7126 (	405		cdbhash_oidvector_ops	PGNSP PGUID ));
DATA(insert OID = 7127 (	405		cdbhash_cash_ops	PGNSP PGUID ));
DATA(insert OID = 7128 (	405		cdbhash_complex_ops	PGNSP PGUID ));
DATA(insert OID = 7129 (	405		cdbhash_uuid_ops	PGNSP PGUID ));
DATA(insert OID = 7130 (	405		cdbhash_enum_ops	PGNSP PGUID ));

#endif   /* PG_OPFAMILY_H */

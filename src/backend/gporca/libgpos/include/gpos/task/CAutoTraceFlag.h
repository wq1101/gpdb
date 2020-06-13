//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2008 Greenplum, Inc.
//
//	@filename: 
//		CAutoTraceFlag.h
//
//	@doc:
//		Auto wrapper to set/reset a traceflag for a scope
//---------------------------------------------------------------------------
#ifndef GPOS_CAutoTraceFlag_H
#define GPOS_CAutoTraceFlag_H

#include "gpos/base.h"
#include "gpos/task/ITask.h"
#include "gpos/task/traceflags.h"
#include "gpos/common/CStackObject.h"


namespace gpos
{
	//---------------------------------------------------------------------------
	//	@class:
	//		CAutoTraceFlag
	//
	//	@doc:
	//		Auto wrapper;
	//
	//---------------------------------------------------------------------------
	class CAutoTraceFlag : public CStackObject
	{
		private:

			// traceflag id
			ULONG m_trace;

		// original value
			BOOL m_orig;

			// no copy ctor
			CAutoTraceFlag(const CAutoTraceFlag&);
			
		public:
		
			// ctor
			CAutoTraceFlag(ULONG trace, BOOL orig);

			// dtor
			virtual
			~CAutoTraceFlag ();

	}; // class CAutoTraceFlag
	
}


#endif // !GPOS_CAutoTraceFlag_H

// EOF


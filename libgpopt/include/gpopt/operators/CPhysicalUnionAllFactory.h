//	Greenplum Database
//	Copyright (C) 2016 VMware, Inc. or its affiliates.
#ifndef GPOPT_CPhysicalUnionAllFactory_H
#define GPOPT_CPhysicalUnionAllFactory_H

#include "gpos/types.h"

#include "gpopt/operators/CLogicalUnionAll.h"
#include "gpopt/operators/CPhysicalUnionAll.h"

namespace gpopt
{
// Constructs a gpopt::CPhysicalSerialUnionAll operator instance.
class CPhysicalUnionAllFactory
{
private:
	CLogicalUnionAll *const m_popLogicalUnionAll;

public:
	CPhysicalUnionAllFactory(CLogicalUnionAll *popLogicalUnionAll);

	CPhysicalUnionAll *PopPhysicalUnionAll(CMemoryPool *mp);
};

}  // namespace gpopt

#endif	//GPOPT_CPhysicalUnionAllFactory_H

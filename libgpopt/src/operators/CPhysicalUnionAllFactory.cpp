//	Greenplum Database
//	Copyright (C) 2016 VMware, Inc. or its affiliates.


#include "gpopt/operators/CPhysicalUnionAllFactory.h"

#include "gpos/base.h"

#include "gpopt/exception.h"
#include "gpopt/operators/CPhysicalSerialUnionAll.h"
#include "gpopt/xforms/CXformUtils.h"

namespace gpopt
{
CPhysicalUnionAllFactory::CPhysicalUnionAllFactory(
	CLogicalUnionAll *popLogicalUnionAll)
	: m_popLogicalUnionAll(popLogicalUnionAll)
{
}

CPhysicalUnionAll *
CPhysicalUnionAllFactory::PopPhysicalUnionAll(CMemoryPool *mp)
{
	CColRefArray *pdrgpcrOutput = m_popLogicalUnionAll->PdrgpcrOutput();
	CColRef2dArray *pdrgpdrgpcrInput = m_popLogicalUnionAll->PdrgpdrgpcrInput();

	// TODO:  May 2nd 2012; support compatible types
	if (!CXformUtils::FSameDatatype(pdrgpdrgpcrInput))
	{
		GPOS_RAISE(gpopt::ExmaGPOPT, gpopt::ExmiUnsupportedOp,
				   GPOS_WSZ_LIT("Union of non-identical types"));
	}

	pdrgpcrOutput->AddRef();
	pdrgpdrgpcrInput->AddRef();

	return GPOS_NEW(mp)
		CPhysicalSerialUnionAll(mp, pdrgpcrOutput, pdrgpdrgpcrInput);
}

}  // namespace gpopt

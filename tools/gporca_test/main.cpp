//---------------------------------------------------------------------------
//	pg_orca
//	Copyright (C) 2024
//
//	@filename:
//		main.cpp
//
//	@doc:
//		Standalone ORCA optimizer test binary.
//
//		Usage:
//		  gporca_test -d <file.mdp>   Execute a minidump and print the plan
//		  gporca_test -d <file.mdp> -p  Print the DXL plan to stdout
//		  gporca_test -d <file.mdp> -i <plan_id>  Enumerate and pick specific plan
//		  gporca_test -T <flag>       Set a trace flag before running
//---------------------------------------------------------------------------

#include <cstdio>
#include <cstring>

#include "gpos/_api.h"
#include "gpos/common/CMainArgs.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpos/task/CAutoTraceFlag.h"
#include "gpos/test/CUnittest.h"
#include "gpos/types.h"

#include "gpopt/engine/CEnumeratorConfig.h"
#include "gpopt/engine/CStatisticsConfig.h"
#include "gpopt/init.h"
#include "gpopt/mdcache/CMDCache.h"
#include "gpopt/minidump/CMinidumperUtils.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "gpopt/xforms/CXformFactory.h"
#include "naucrates/init.h"
#include "naucrates/dxl/CDXLUtils.h"

using namespace gpos;
using namespace gpopt;
using namespace gpdxl;

// Default number of segments for single-node mode
#define GPORCA_TEST_SEGMENTS 1

static ULONG tests_failed = 0;

//---------------------------------------------------------------------------
//
//	PvExec
//		Main execution function run inside the GPOS task framework.
//
//---------------------------------------------------------------------------
static void *
PvExec(void *pv)
{
	CMainArgs *pma = (CMainArgs *) pv;

	CHAR ch = '\0';
	CHAR *file_name = nullptr;
	BOOL fMinidump = false;
	BOOL fPrintDXLPlan = false;
	ULLONG ullPlanId = 0;

	while (pma->Getopt(&ch))
	{
		switch (ch)
		{
			case 'd':
				fMinidump = true;
				file_name = optarg;
				break;

			case 'p':
				fPrintDXLPlan = true;
				break;

			case 'T':
				CUnittest::SetTraceFlag(optarg);
				break;

			case 'i':
				ullPlanId = CUnittest::UllParsePlanId(optarg);
				GPOS_SET_TRACE(EopttraceEnumeratePlans);
				break;

			default:
				break;
		}
	}

	if (!fMinidump)
	{
		(void) fprintf(stderr,
					   "Usage: gporca_test -d <file.mdp> [-p] [-i <plan_id>] [-T <trace_flag>]\n"
					   "\n"
					   "  -d <file.mdp>    Execute a minidump file\n"
					   "  -p               Print the resulting DXL plan to stdout\n"
					   "  -i <plan_id>     Select specific plan ID (enables plan enumeration)\n"
					   "  -T <flag>        Set a trace flag (integer)\n");
		tests_failed = 1;
		return nullptr;
	}

	// Initialize DXL support and metadata cache
	InitDXL();
	CMDCache::Init();

	{
		CAutoMemoryPool amp;
		CMemoryPool *mp = amp.Pmp();

		// Load the minidump file
		CDXLMinidump *pdxlmd = CMinidumperUtils::PdxlmdLoad(mp, file_name);
		GPOS_CHECK_ABORT;

		// Get optimizer config from the dump, or create a default one
		COptimizerConfig *optimizer_config = pdxlmd->GetOptimizerConfig();
		if (nullptr == optimizer_config)
		{
			optimizer_config = COptimizerConfig::PoconfDefault(mp);
		}
		else
		{
			optimizer_config->AddRef();
		}

		// Override plan ID if given on command line
		if (ullPlanId != 0)
		{
			optimizer_config->GetEnumeratorCfg()->SetPlanId(ullPlanId);
		}

		// Determine segment count (single-node: 1)
		ULONG ulSegments = GPORCA_TEST_SEGMENTS;
		if (nullptr != optimizer_config->GetCostModel())
		{
			ULONG ulSegs = optimizer_config->GetCostModel()->UlHosts();
			if (ulSegments < ulSegs)
				ulSegments = ulSegs;
		}

		// Execute the minidump: parse query DXL + metadata, run ORCA, return plan DXL
		CDXLNode *pdxlnPlan = CMinidumperUtils::PdxlnExecuteMinidump(
			mp, file_name, ulSegments, 1 /*ulSessionId*/, 1 /*ulCmdId*/,
			optimizer_config, nullptr /*pceeval*/);
		GPOS_CHECK_ABORT;

		if (fPrintDXLPlan)
		{
			CAutoTrace at(mp);
			CDXLUtils::SerializePlan(
				mp, at.Os(), pdxlnPlan,
				optimizer_config->GetEnumeratorCfg()->GetPlanId(),
				optimizer_config->GetEnumeratorCfg()->GetPlanSpaceSize(),
				true /*serialize_header_footer*/,
				true /*indentation*/);
		}

		// Print a success summary regardless
		{
			CAutoTrace at(mp);
			at.Os() << "Minidump executed successfully: " << file_name;
		}

		GPOS_DELETE(pdxlmd);
		optimizer_config->Release();
		pdxlnPlan->Release();
	}

	CMDCache::Shutdown();
	return nullptr;
}


//---------------------------------------------------------------------------
//
//	main
//
//---------------------------------------------------------------------------
INT
main(INT iArgs, const CHAR **rgszArgs)
{
	struct gpos_init_params gpos_params = {nullptr};
	gpos_init(&gpos_params);
	gpdxl_init();
	gpopt_init();

	GPOS_ASSERT(iArgs >= 0);

	CMainArgs ma(iArgs, rgszArgs, "d:pT:i:");

	gpos_exec_params params;
	params.func = PvExec;
	params.arg = &ma;
	params.stack_start = &params;
	params.error_buffer = nullptr;
	params.error_buffer_size = -1;
	params.abort_requested = nullptr;

	if (gpos_exec(&params) || (tests_failed != 0))
	{
		return 1;
	}

	return 0;
}

// EOF

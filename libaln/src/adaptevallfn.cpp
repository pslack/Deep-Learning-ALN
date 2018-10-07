// ALN Library
// Copyright (C) 2018 William W. Armstrong.
// 
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// Version 3 of the License, or (at your option) any later version.
// 
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
// 
// For further information contact 
// William W. Armstrong

// 3624 - 108 Street NW
// Edmonton, Alberta, Canada  T6J 1B4

// adaptevallfn.cpp

///////////////////////////////////////////////////////////////////////////////
//  File version info:
// 
//  $Archive: /ALN Development/libaln/src/adaptevallfn.cpp $
//  $Workfile: adaptevallfn.cpp $
//  $Revision: 6 $
//  $Date: 8/18/07 3:01p $
//  $Author: Arms $
//
///////////////////////////////////////////////////////////////////////////////

#ifdef ALNDLL
#define ALNIMP __declspec(dllexport)
#endif

#include <aln.h>
#include "alnpriv.h"

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

///////////////////////////////////////////////////////////////////////////////
// LFN specific eval
//  - returns distance of LFN from point and also sets the 
//    node's dblDistance member for use by adaptive routines

double ALNAPI AdaptEvalLFN(ALNNODE* pNode, ALN* pALN, const double* adblX,
                           ALNNODE** ppActiveLFN)
{
  ASSERT(adblX != NULL);
  ASSERT(pALN != NULL);
  ASSERT(pNode != NULL);
  ASSERT(ppActiveLFN != NULL);
  ASSERT(NODE_ISLFN(pNode));

  ASSERT(LFN_VARMAP(pNode) == NULL);      // var map not yet supported
  ASSERT(LFN_VDIM(pNode) == pALN->nDim);  // no different sized vectors yet

  *ppActiveLFN = pNode;

  // set node eval flag
  NODE_FLAGS(pNode) |= NF_EVAL;

  // calc dist of point from line
  int nDim = pALN->nDim;
  const double* adblW = LFN_W(pNode);
  double dblA = *adblW++;                 // skip past bias weight       
  for (int i = 0; i < nDim; i++)     
  {
    dblA += adblW[i] * adblX[i];
  }
  
  NODE_DISTANCE(pNode) = dblA;
  
  return dblA;
}

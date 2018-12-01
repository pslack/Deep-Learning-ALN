// ALN Library
// file train_ops.cpp
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

#ifdef ALNDLL
#define ALNIMP __declspec(dllexport)
#endif

#include <aln.h>
#include "alnpriv.h"

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// include files
#include <stdafx.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <alnpp.h>
#include <dtree.h>
#include <datafile.h>
#include <malloc.h>
#include ".\cmyaln.h" 
#include "alnextern.h"
#include "alnintern.h"
#include <Eigen/Dense>

using namespace Eigen;

//defines used to set up TRfile and VARfile

#define LINEAR_REGRESSION 0
#define OVERTRAINING1 1
#define OVERTRAINING2 2
#define APPROXIMATION 3
#define BAGGING 4

// We use dblRespTotal in two ways and the following definition helps.
#define DBLNOISEVARIANCE dblRespTotal

// files used in training operations
CDataFile TRfile;
CDataFile VARfile;
CDataFile TRfile1;
CDataFile TRfile2;

//routines
void ALNAPI doLinearRegression(); // Determines an upper bound on error, and provides a start for other training.
void ALNAPI createNoiseVarianceFile(); // This does overtraining and creates the noise variance file VARfile.
void ALNAPI approximate(); // Actually does training avoiding overtraining using samples in VARfile.
void ALNAPI outputTrainingResults();
void ALNAPI trainAverage(); // Takes several ALNs created in approximate() and creates an ALN of their average
void ALNAPI constructDTREE(int nMaxDepth); // Takes the average ALN and turns it into a DTREE
void ALNAPI cleanup(); // Destroys ALNs etc.
void fillvector(double * adblX, CMyAln* paln); // Sends a vector to training from a file, online or averaging.
void ALNAPI createTR_VARfiles(int nChoose);
void createSamples(int nOTTR, CMyAln* pOTTR); // Creates noise variance samples for two overtrainings 1 & 2
void prepareQuickStart(CMyAln* pALN);

// ALN pointers

static CMyAln* pALN = NULL; // declares a pointer to an ALN used in linear regression
static CMyAln* pOTTR = NULL; // ALNs overtrained on disjoint parts of TVfile to get noise variance samples
static CMyAln** apALN = NULL;  // an array of pointers to ALNs used in approximate()
static CMyAln* pAvgALN = NULL;      // an ALN representing the bagged average of several ALNs trained on the TVfile with different random numbers

// Some global variables
double dblMinRMSE = 0; // stops training when the training error is smaller than this
double dblLearnRate = 0.2;  // roughly, 0.2 corrects most of the error for if we make 15 passes through TRfile
int nMaxEpochs = 10; // if the learnrate is 0.2, then one will need 5 or 10 roughly to almost correct the errors
long nRowsTR; // the number of rows in the current training set loaded into TRfile
long nRowsVAR; // the number of rows in the noise variance file.  When approximation starts, this should be nRowsTV
long nRowsSet1; // The number nRowsTV/2 of rows of TRfile1 used for overtraining on Set 1.
double dblFlimit = 0 ;// For linear regression can be anything, for overtraining must be zero
int nEpochSize; // the number of input vectors in the current training set
BOOL bALNgrowable = TRUE; // FALSE for linear regression, TRUE otherwise
BOOL bOvertrain = FALSE; // Controls setup of training for creating two overtrainings of a partition of the TVfile.
BOOL bStopTraining; // Set to TRUE and becomes FALSE if any (active) linear piece needs training
int nNotifyMask = AN_TRAIN | AN_EPOCH | AN_VECTORINFO; // used with callbacks
double * adblX = NULL;
ALNNODE** apLFN; // This stores the LFN of the overtraining a sample X belongs to.
long nNoiseVarianceSamples; // The number of noise variance samples in VARfile

void ALNAPI doLinearRegression() // routine
{
  fprintf(fpProtocol,"\n****** Linear regression begins: it gains useful information for further training *****\n");
	fflush(fpProtocol); 
	// If something goes wrong, we can perhaps see it in the protocol file if we flush before the crash.
	// This iterative algorithm is not an accepted method in general, but works well here.
	// The reason for using it for ALN approximation is that the linear regression
	// problem for a linear piece (leaf node = LFN) is constantly changing. A piece gains or loses input vectors (samples).
	// for which it determines the output value via the ALN tree of max and min operators.
	// This constantly changes the samples which are to be least-squares fitted for a given linear piece.
	// Linear regression helps get the centroid and weights of a linear piece in neighborhood of
	// good values to start other training later.
	// Set up a sample buffer.
	adblX = (double *)malloc((nDim) * sizeof(double));
	// Set up the ALN
	pALN = new CMyAln;
	if (!(pALN->Create(nDim, nDim-1))) // nDim is the number of inputs to the ALN plus one for
		//the ALN output. The default output variable is nDim-1.
	{
		fprintf(fpProtocol,"Stopping: linear regression ALN creation failed!\n");
    fflush(fpProtocol);
		exit(0);
	}
	// Set constraints on variables for the ALN
  // createTR_VARfiles(0); see ALNfitDeepView; for linear regression, training set TRfile is  about 50% of the TVfile.
	// The rest is for noise variance samples in VARfile, not used until the approximation phase below.
	// NB The region concept has not been completed.  It allows the user to impose constraints
	// e.g. on slopes(weights) which differ in different parts of the domain.  All we have is region 0 now.
	bALNgrowable = FALSE; // The ALN consists of one leaf node LFN for linear regression.
	bOvertrain = FALSE; // TRUE only during overtraining.
	bTrainingAverage = FALSE; // Switch to tell fillvector whether get a training vector or compute an average.
	prepareQuickStart(pALN);
	nMaxEpochs = 15; // nMaxEpochs is the number of passes through the data (epochs); if the tree is
	// growable, this is epochs before each splitting. For linear regression, it is just a number of epochs.
	dblMinRMSE = 0; // Don't stop early because of low training error.
	dblLearnRate = 0.15;  // This rate seems ok for linear regression.
	// Set up the data
	createTR_VARfiles(LINEAR_REGRESSION);
	int nEpochSize = TRfile.RowCount();	// nEpochsize gives the number of training samples. Later nRowsVAR=nRowsTR.
	int nColumns = TRfile.ColumnCount(); // This is always nDim for training.
	ASSERT(nColumns == nDim);
	const double* adblData = TRfile.GetDataPtr();
	// The following could also set NULL instead of adblData which leads
	// to fillvector() below controlling the input vectors to the ALN.  fillvector() allows the
	// system to choose training vectors more flexibly, even online.
	// The advantage of giving the pointer adblData is that epochs consistently
	// permute the order of the samples and go through all samples exactly once per epoch.
	pALN->SetDataInfo(nEpochSize, nColumns, adblData, NULL);
	
	// TRAIN FOR LINEAR REGRESSION   vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
	// The reason for iterations is so that we can monitor progress in the ... TrainProtocol.txt file,
	// and set new parameters for subsequent training.

	for (int iteration = 0; iteration < 100; iteration++)  // experimental
	{
		if (!pALN->Train(nMaxEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask))
		{
			fprintf(fpProtocol, "Linear regression training failed!\n");
			fflush(fpProtocol);
			exit(0);
		}
		fprintf(fpProtocol, "\nIteration %d of linear regression completed. Training RMSE = %f \n",iteration, dblTrainErr);
	}
	fprintf(fpProtocol, "Linear regression training succeeded!\n");
	fflush(fpProtocol);
  // We should now have a good linear regression fit.
  // Find the weights on the linear piece using an evaluation at the 0 vector (could be any other place!).
	
	ALNNODE* pActiveLFN;
  for(int m=0; m < nDim; m++)
	{
		adblX[m] = 0;
	}
	double dummy = pALN->QuickEval(adblX, &pActiveLFN); // this finds a pointer to the linear piece
	fprintf(fpProtocol,"Linear regression weights on ALN\n");
	for (int m = 0; m < nDim-1; m++)
	{
    if(nLag[m] == 0) // any data we are looking at can have nonzero lags
    {
			// Note that adblW is stored in a different way, the weight on axis 0 is in adblW[1] etc.
  		fprintf(fpProtocol,"Linear regression weight on %s is %f centroid is %f\n",\
			varname[nInputCol[m]] , ((pActiveLFN)->DATA.LFN.adblW)[m+1], ((pActiveLFN)->DATA.LFN.adblC)[m]); 
		}
    else
    {
  		fprintf(fpProtocol,"Linear regression weight on ALN input %s@lag%d is %f centroid is %f\n",\
			varname[nInputCol[m]], nLag[m] , ((pActiveLFN)->DATA.LFN.adblW)[m+1], ((pActiveLFN)->DATA.LFN.adblC)[m]); 
    }
		adblLRC[m] = ((pActiveLFN)->DATA.LFN.adblC)[m]; // centroid of linear piece for axis m
		adblLRW[m+1] = ((pActiveLFN)->DATA.LFN.adblW)[m+1]; // linear regression weight for axis m
  }
	fprintf(fpProtocol,"Value of the linear piece at the origin is %f; the value at the centroid is %f\n",\
		((pActiveLFN)->DATA.LFN.adblW)[0], ((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] ); 
	fflush(fpProtocol);
	// Put the negative sum of weighted centroids into the 0 position
	// compress the weighted centroid info into W[0], i.e. take the centroids out of all terms like
	// ... + weight_m*(x_m-centroid_m) + ... to have just one number in W[0]
	adblLRW[0] = adblLRC[nDim -1] = ((pActiveLFN)->DATA.LFN.adblC)[nDim -1];
  for (int m = 0; m < nDim - 1; m++)
  {
    adblLRW[0] -= adblLRW[m+1] * adblLRC[m];
  }
	adblLRW[nDim] = -1.0;  // the -1 weight on the ALN output may make a difference somewhere, so we set it here.
	// The idea of weight = -1 is that the hyperplane equation can be written 
	// a*x + b*y -1*z = 0 like an equation instead of z = a*x + b*y like a function.
	// This representation is good if we want to invert the ALN, i.e. get x as a function of y and z.
	// How to do it? Just multiply the first equation by -1/a in all linear pieces. The weight on the output becomes -1.0
	// This is not fully implemented yet.  The output of the ALN is now always the highest index ALN variable nDim -1.

	double desired, predict, se;
	int nCount; 
	nCount = 0;
	for (int j = 0; j < nRowsTR; j++)
	{
		for (int i = 0; i < nDim; i++)
		{
			adblX[i] = TRfile.GetAt(j, i, 0);
		}
		desired = adblX[nDim - 1]; // get the desired result
		adblX[nDim - 1] = 0; // not used in evaluation by QuickEval
		predict = pALN->QuickEval(adblX, &pActiveLFN);
		se = (predict - desired) * (predict - desired);
		nCount++;
		dblLinRegErr +=se;
	} // end loop over  the training file
	dblLinRegErr = sqrt(dblLinRegErr / nCount); 
	fprintf(fpProtocol, "Root Mean Square Linear Regression Training Error is %f \n", dblLinRegErr);
	fprintf(fpProtocol, "The number of data points used in determining linear regression error is %d \n",nCount);
	fflush(fpProtocol);
	pALN->Destroy();
	free(adblX);
	// We are finished with that ALN and have destroyed it
	fprintf(fpProtocol,"Linear regression complete\n");
	fflush(fpProtocol);
}

void ALNAPI createNoiseVarianceFile() // routine
	// This routine creates two overtrained ALNs on disjoint parts of TRfile
	// and creates noise variance samples in VARfile.
{
	fprintf(fpProtocol, "\n*** Overtraining for use with noise variance estimation begins***\n");
	fflush(fpProtocol);
	// Allocate space for samples
	adblX = (double *)malloc((nDim) * sizeof(double));
	for (int nOTTR = 1; nOTTR <= 2; nOTTR++) // We train two ALNs (using the same pointer name)
		// on a partition of of TVfile into two equal (+ or - 1) subsets. Each trained ALN is used
		// as a basis of comparison to the samples not used in its creation. 
	{
		pOTTR = new CMyAln;
		if (!(pOTTR->Create(nDim, nDim - 1))) //create an overtraining ALN
		{
			fprintf(fpProtocol, "ALN creation for overtraining failed!\n");
			fflush(fpProtocol);
			exit(0);
		}
		if (!pOTTR->SetGrowable(pOTTR->GetTree()))
		{
			fprintf(fpProtocol, "Setting overtraining ALN growable failed!\n");
			fflush(fpProtocol);
			exit(0);
		}
		fprintf(fpProtocol, "\nStart new overtraining **************************************\n");
		fflush(fpProtocol);
		prepareQuickStart(pOTTR); // We have to avoid constraints that stand in the way of overfitting!
		nNumberLFNs = 1;  // The ALN is just one leaf node to start.
		// Training booleans
		bALNgrowable = TRUE; // This is TRUE for all training except Linear Regression (LR).
		bStopTraining = FALSE; // This is set to FALSE but becomes TRUE and stops training when all leaf nodes fit well.
		bOvertrain = TRUE; // This is TRUE only for overtraining of subsets of the TVfile to relax weight constraints.
		bTrainingAverage = FALSE; // When this is TRUE, it tells fillvector compute an average of values of approximation results.
		bJitter = FALSE; // When this is TRUE, the position of a training sample in the domain is moved about in a small volume.
		if (bJitter) fprintf(fpProtocol, "Jitter is used during overtraining for noise variance estimation\n");
		fflush(fpProtocol);
		// (pOTTR->GetRegion(0))->dblSmoothEpsilon = 1.0; // A bit of smoothing so pieces share points can be beneficial. 
		nMaxEpochs = 5; // This is number of epochs before splitting. It has to be greater than 1/dblLearnRate.
		dblMinRMSE = 0.0; // We don't stop overtraining upon reaching even a very low training error.
		dblLearnRate = 0.2; // This is, roughly, the fraction of the training error that is corrected per sample.
		dblFlimit = 0.001;// F-test limit for splitting. If dblFlimit <= 1.0,
		// then dblFlimit is compared directly to training MSE. If MSE is greater, splitting occurs. 
		fprintf(fpProtocol, "OTTR learning rate %f, Smoothing %f, split if piece MSE > %f\n", dblLearnRate,
			(pOTTR->GetRegion(0))->dblSmoothEpsilon, dblFlimit);
		fflush(fpProtocol);
		if (nOTTR == 1) // We inform the ALN that only about half the samples are used in each overtraining.
		{
			createTR_VARfiles(OVERTRAINING1);
			long nRowsTR1 = TRfile1.RowCount();
			ASSERT(nRowsTR1 == nRowsSet1);
			nRowsTR = nRowsTR1; // Temporarily needed to communicate with routines that use nRowsTR
			const double* adblData1 = TRfile1.GetDataPtr();
			pOTTR->SetDataInfo(nRowsTR1, nDim, adblData1, NULL);
		}
		else // nOTTR == 2
		{
			createTR_VARfiles(OVERTRAINING2);
			long nRowsTR2 = TRfile2.RowCount();
			nRowsTR = nRowsTR2; // Temporarily needed to communicate with routines that use nRowsTR
			const double* adblData2 = TRfile2.GetDataPtr();
			pOTTR->SetDataInfo(nRowsTR2, nDim, adblData2, NULL);
		}
		int nNumberIterations = 120; // TO DO:experiment
		// The reason for iterations is so that we can monitor progress between splittings in TrainProtocol.txt,
		// and set new parameters for further training. Reporting is before the splitting.
		for (int iteration = 0; iteration < nNumberIterations; iteration++)  // experimentation required!
		{
			// Overtrain on some data to take the difference with samples not used to create noise variance samples.
			// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
			if (!pOTTR->Train(nMaxEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask))
			{
				fprintf(fpProtocol, "OTTR overtraining failed!\n");
				fflush(fpProtocol);
				exit(0);
			}
			if (bStopTraining == TRUE)
			{
				fprintf(fpProtocol, "Iterations stopped at %d because all leaf nodes have stopped changing!\n", iteration);
				fflush(fpProtocol);
				bStopTraining = FALSE;
				break;
			}
		}
		fprintf(fpProtocol, "\nOvertraining %d completed. Training RMSE = %f \n", nOTTR, dblTrainErr);
		fflush(fpProtocol);
		createSamples(nOTTR, pOTTR);
		// We don't need the overtraining ALN any more (but it can reincarnate once!)
		pOTTR->Destroy();
		pOTTR = NULL;
	}
	free(adblX);
	nRowsTR = TRfile.RowCount(); // Restore nRowsTR to its proper value
	// We keep the VARfile for the noise level F-tests determining piece-splitting during approximation.
  // In future versions of the program we will create a weight-bounded ALN to learn the noise variance
  // as an ALN function and store it for the present and future evaluations.
}

void ALNAPI approximate() // routine
{
	fprintf(fpProtocol, "\n**************Approximation with one or more ALNs begins ********\n");
	fflush(fpProtocol);
	int nalns = nALNs;  // The number of ALNs over which we average (for "bagging")
	// createTR_VARfiles(APPROXIMATION);  // prepares for using the whole TVfile and the whole VARfile with noise variance samples for training and stopping
	fprintf(fpProtocol,"Training %d approximation ALNs starts, using F-test to stop splitting\n", nalns);
	fflush(fpProtocol);
	if(bJitter)
	{
		fprintf(fpProtocol, "Jitter is used during approximation\n");
	}
	else
	{
		fprintf(fpProtocol, "Jitter is not used during approximation\n");
	}
	fflush(fpProtocol);
// Explanation of dblFlimit
// dblFlimit = 2.59 says that splitting of a linear piece is prevented when the mean square
// training error of a piece becomes less than 2.59 times the average of the noise variance
// samples on it. This value comes from tables of the F-test for d.o.f. > 7 and probability 90%.
// For 90% with 3 d.o.f the value is 5.39, i.e. with fewer d.o.f. training stops sooner
// and the training error will generally be larger than with a lower F-value.
// We have to IMPROVE the program to use d.o.f. of *each* piece for both training error
// and noise variance. In the present setup, the d.o.f of the two are equal.
	const double adblFconstant[13]{ 9.00, 5.39, 4.11, 3.45, 3.05, 2.78, 2.59, 2.44, 2.32, 1.79, 1.61, 1.51, 1.40 };
	int dofIndex;
	dofIndex = nDim - 2; // the lowest possible nDim is 2 for one ALN input and one ALN output
	if(nDim > 10) dofIndex = 8;
	if(nDim > 20) dofIndex = 9;
	if(nDim > 30) dofIndex = 10;
	if(nDim > 40) dofIndex = 11;
	if(nDim > 60) dofIndex = 12;

	dblFlimit = adblFconstant[dofIndex]; // This can determine splitting for linear pieces
	dblFlimit = 1.4; // Override for stopping splitting, but temporary until we can compute the dof for the pieces
	// from the training samples and noise variance samples.
	// Other values for dblFlimit with other numbers of samples, i.e. degrees of freedom, are:
	// n dblFlimit
	// 2 9.00
	// 3 5.39
	// 4 4.11
	// 5 3.45
	// 6 3.05
	// 7 2.78
	// 8 2.59
	// 9 2.44
	// 10 2.32
	// 20 1.79
	// 30 1.61
	// 40 1.51
	// 60 1.40
	// 120 1.26
	// REQUIRED IMPROVEMENT  We have to take into account the actual numbers of samples of TSfile and VARfile per leaf node during an epoch.
	// As training of the approximant progresses, the dof of pieces decreases and dblFlimit should be appropriate.
	fprintf(fpProtocol, "nDim is %d and the F-limit used for stopping splitting is %f \n", nDim, dblFlimit);
	fflush(fpProtocol);
	// ***************** SET UP THE ARRAY OF POINTERS TO ALNS FOR TRAINING ONLY *************************
	if(bTrain)
	{
		apALN = (CMyAln**) malloc(nALNs * sizeof(CMyAln*));
	}
	fflush(fpProtocol);

	for (int n = 0; n < nalns; n++)
	{
		 apALN[n] = new CMyAln; // NULL initialized ALN
	}
	// Set up the sample buffer.
	double * adblX = (double *)malloc((nDim) * sizeof(double));
	// Train all the ALNs
	for (int n = 0; n < nalns; n++)
	{
		// Set up the ALN, index n
		if (!(apALN[n]->Create(nDim, nDim-1)))
		{
	    fprintf(fpProtocol,"ALN creation failed!\n");
      fflush(fpProtocol);
			exit(0);
		}
		// Now make the tree growable
		if (!apALN[n]->SetGrowable(apALN[n]->GetTree()))		
		{
	    fprintf(fpProtocol,"Setting ALN %d growable failed!\n", n);
      fflush(fpProtocol);
      exit(0);
		}
		bALNgrowable = TRUE; // now the nodes can split
		// Set constraints on variables for ALN index n
		prepareQuickStart(apALN[n]);
		bTrainingAverage = FALSE;
		bOvertrain = FALSE;
		if(bClassify)
    {
     // TO DO
    }
		(apALN[n]->GetRegion(0))->dblSmoothEpsilon = 0.0;
		fprintf(fpProtocol, "The smoothing for training each approximation is %f\n", 0.0); 
		if(bEstimateNoiseVariance)
		nMaxEpochs = 100;
		dblMinRMSE = 0.0;
		dblLearnRate = 0.15;
		bStopTraining = FALSE; // Set TRUE in alntrain.cpp. Set FALSE by any piece needing more training. 
    nNumberLFNs = 1;  // initialize at 1
		// Set up the data
		createTR_VARfiles(APPROXIMATION);
		// Tell the training algorithm the way to access the data using fillvector
		nRowsTR = TRfile.RowCount();
		const double* adblData = TRfile.GetDataPtr();
		apALN[n]->SetDataInfo(nRowsTR, nDim, adblData, NULL);
		fprintf(fpProtocol,"----------  Training approximation ALN %d ------------------\n",n);
		fflush(fpProtocol);
		for(int iteration = 0; iteration < 40; iteration++) // is 40 iterations enough?
		{
			fprintf(fpProtocol, "\nStart iteration %d of approximation with ALN %d, learning rate %f\n", iteration,
				n, dblLearnRate);
			fflush(fpProtocol);

			// TRAIN ALNS WITHOUT OVERTRAINING   vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
			if (!apALN[n]->Train(nMaxEpochs, dblMinRMSE, dblLearnRate, bJitter, nNotifyMask))
			{
			  fprintf(fpProtocol,"Training failed!\n");
        exit(0);
			}
			if(bEstimateNoiseVariance)
      {
				fprintf(fpProtocol,"Training RMSE = %f\n", dblTrainErr);
			}
			fprintf(fpProtocol,"Number of active LFNs = %d. Tree growing\n", nNumberLFNs);
			fflush(fpProtocol);
			if (bStopTraining == TRUE)
			{
				fprintf(fpProtocol, "\nTraining of approximation ALN %d completed at iteration %d \n", n, iteration);
				fprintf(fpProtocol, "This training stopped because all leaf nodes have stopped changing!\n");
				bStopTraining = FALSE;
				fflush(fpProtocol);
				break;
			}
			fflush(fpProtocol);
		} // end of loop of training interations over one ALN
  } // end of the loop for n = ALN index
	free(adblX);
	// we don't destroy the ALNs because they are needed for further work in reporting
}

void ALNAPI outputTrainingResults() // routine
{
	fprintf(fpProtocol, "\n**** Analyzing results on the training/variance set begins ***\n");
	// all the ALNs have been trained, now report results
	int i, j, k, n;
	double desired, average, sum;
	int nalns; // lower case indicates a value on the stack
	nalns = nALNs;
	ALNNODE* pActiveLFN = NULL;
	// test the average of the ALNs against data in the TV set
	double * adblX = (double *)malloc((nDim) * sizeof(double));
	double * adblWAcc = (double *)malloc((nDim) * sizeof(double));
	double * adblAbsWAcc = (double *)malloc((nDim) * sizeof(double));
	for (k = 0; k < nDim; k++)
	{
		adblWAcc[k] = 0; // zero the accumulator for the average weight
		adblAbsWAcc[k] = 0; // zero the accumulator for the average absolute weight
	}
	double se = 0; // square error accumulator

	int	nClassError = 0;  // for classification problems
	for (j = 0; j < nRowsTV; j++)
	{
		sum = 0;

		for (n = 0; n < nalns; n++)
		{
			for (i = 0; i < nDim; i++)
			{
				adblX[i] = TVfile.GetAt(j, i, 0);
			}
			double dblValue = apALN[n]->QuickEval(adblX, &pActiveLFN);
			sum += dblValue;
			for (int k = 0; k < nDim; k++)
			{
				adblWAcc[k] += ((pActiveLFN)->DATA.LFN.adblW)[k + 1]; //the adblW vector has the bias in it
																												// so the components are shifted
				adblAbsWAcc[k] += fabs(((pActiveLFN)->DATA.LFN.adblW)[k + 1]);
			}
		}
		average = sum / (double)nalns; // this is the result of averaging [0,1]-limited ALNs
		desired = TVfile.GetAt(j, nDim - 1, 0); // get the desired result	
		se += (average - desired) * (average - desired);
		if (((desired > 0.5) && (average < 0.5)) || ((desired < 0.5) && (average > 0.5))) nClassError++;
	}
	double rmse = sqrt(se / ((double)nRowsTV - 1.0)); // frees se for use below.
	// get the average weight on all variables k
	for (k = 0; k < nDim; k++)
	{
		adblWAcc[k] /= (nRowsTV * nalns);
		adblAbsWAcc[k] /= (nRowsTV * nalns);
	}
	fprintf(fpProtocol, "Size of datasets PP TV Test %d  %d  %d \n", nRowsPP, nRowsTV, nRowsTS);
	fprintf(fpProtocol, "Root mean square error of the average over %d ALNS is %f \n", nalns, rmse);
	fprintf(fpProtocol, "Warning: the above result is optimistic, see results on the test set below\n");
	fprintf(fpProtocol, "Importance of each input variable:\n");
	fprintf(fpProtocol, "Abs imp = stdev(input var) * average absolute weight / stdev(output var) \n");
	fprintf(fpProtocol, "Abs imp is numerical and indicates ups and downs in output when the given input varies.\n");
	fprintf(fpProtocol, "For example a sawtooth function with six teeth would have importance 12.\n");
	fprintf(fpProtocol, "First we have to compute the standard deviation of the output variable.\n");
	fflush(fpProtocol);
	//compute the average of the output variable in the TVset
	k = nDim - 1;
	desired = 0;
	for (j = 0; j < nRowsTV; j++)
	{
		desired += TVfile.GetAt(j, k, 0);
	}
	desired /= nRowsTV; // now desired holds the average for variable k

	// compute the standard deviation of the output variable in the TVset
	se = 0;
	double temp;
	for (j = 0; j < nRowsTV; j++)
	{
		temp = TVfile.GetAt(j, k, 0);
		se += (temp - desired) * (temp - desired);
	}
	se /= ((double)nRowsTV - 1.0); // sample variance of the output variable
	double stdevOutput = sqrt(se);
	fprintf(fpProtocol, "\nStandard deviation of the output in the TVfile %f\n", stdevOutput);
	if (fabs(stdevOutput) < 1e-10)
	{
		fprintf(fpProtocol, "\nStopping: The standard deviation of the output on the TV set is near 0.\n");
		fclose(fpProtocol);
		exit(0);
	}
	// we compute the variance of each column of TV
	se = 0;
	for (k = 0; k < nDim - 1; k++) // do each variable k
	{
		//compute the average of variable k in TVset
		desired = 0;
		for (j = 0; j < nRowsTV; j++)
		{
			desired += TVfile.GetAt(j, k, 0);
		}
		desired /= nRowsTV; // now desired holds the average for variable k

		// compute the standard deviation of variable k in TVset
		se = 0;
		double temp;
		for (j = 0; j < nRowsTV; j++)
		{
			temp = TVfile.GetAt(j, k, 0);
			se += (temp - desired) * (temp - desired);
		}
		se /= ((double)nRowsTV - 1.0); // sample variance of variable k
		dblImportance[k] = sqrt(se) * adblAbsWAcc[k] / stdevOutput;
		if (nLag[k] == 0)
		{
			fprintf(fpProtocol, "Variable %s: stdev = \t%f; avg.wt = \t%f; abs imp = \t%f\n",
				varname[nInputCol[k]], sqrt(se), adblWAcc[k], dblImportance[k]);
		}
		else
		{
			fprintf(fpProtocol, "Variable %s@lag%d: stdev = \t%f; avg.wt = \t%f; abs imp = \t%f\n",
				varname[nInputCol[k]], nLag[k], sqrt(se), adblWAcc[k], dblImportance[k]);
		}
		// we use the product of the variance of k and the average absolute weight as a measure of importance
	}
	if (bClassify)
	{
		fprintf(fpProtocol, "Number of TV file cases misclassified = %d out of %d\n", nClassError, nRowsTV);
		fprintf(fpProtocol, "Percentage of TV file cases misclassified = %f", 100.0*(double)nClassError / (double)nRowsTV);
	}
	free(adblX);
	free(adblWAcc);
	free(adblAbsWAcc);
	fflush(fpProtocol);
}

void ALNAPI trainAverage() // routine
{
  int nalns;
  nalns = nALNs;
	bTrainingAverage = TRUE;
	// For training the average, we use the TV set to
	// define the region where the data points are located.
	// The values of those points are automatically jittered to cover that region.
	fprintf(fpProtocol,"\n**** Training an ALN by resampling the average of approximations ******\n");
	fprintf(fpProtocol, "The F-limit used for stopping splitting of the average ALN is %f \n", dblFlimit);
	// The noise variance values in VARfile are the previous ones divided by nalns.
	createTR_VARfiles(BAGGING);
	pAvgALN = new CMyAln; // NULL initialized ALN
	if (!(pAvgALN->Create(nDim, nDim-1) &&
				pAvgALN->SetGrowable(pAvgALN->GetTree())))
	{
    fprintf(fpProtocol,"Stopping: Growable average ALN creation failed!\n");
    exit(0);
	}
	bALNgrowable = TRUE;
	// Get epsilons, centroids and weights in neighborhood of good values to start training
	prepareQuickStart(pAvgALN);
	fprintf(fpProtocol,"Smoothing epsilon same as for approximation\n\n");
	fflush(fpProtocol);
	// Tell the training algorithm about the data, in particular that fillvector must be used (second last NULL)
	nRowsTR = TRfile.RowCount();
	pAvgALN->SetDataInfo(nRowsTR, nDim, NULL, NULL);
	dblMinRMSE = 0; // Stopping splitting uses the F-test
	dblLearnRate = 0.2;
	//*********
	if(bEstimateNoiseVariance)
	{
		nMaxEpochs = 10;
		// TRAIN AVERAGE ALN vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
		if(!pAvgALN->Train(nMaxEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask)) // fast change to start, dblLearnRate 1.0 for 2 epochs
		{
			 fprintf(fpProtocol,"Training failed!\n");
		}
	}
	else // we have opened a .fit file
	{
		// use the weights and centroids from linear regression to start
		ALNNODE* pActiveLFN;
		pActiveLFN = pAvgALN->GetTree();
		for(int m = 0; m < nDim - 1; m++)
		{
			((pActiveLFN)->DATA.LFN.adblC)[m] = adblLRC[m];
			((pActiveLFN)->DATA.LFN.adblW)[m+1] = adblLRW[m+1];
		}
		((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] = adblLRC[nDim - 1];
		((pActiveLFN)->DATA.LFN.adblW)[0] = adblLRW[0];
		((pActiveLFN)->DATA.LFN.adblW)[nDim] = -1.0;
	}
	//**********	// at this point we have no further use for the starting values from linear regression
	if(bEstimateNoiseVariance)
	{
		delete [] adblLRC; // delete these from free store
		delete [] adblLRW;
		adblLRC = NULL; // set the pointers to NULL
		adblLRW = NULL;
	}
	dblLearnRate = 0.2;
	nEpochSize = nRowsTV; //training average ALN
  nNumberLFNs = 1;  // initialize at 1
  for(int iteration = 0; iteration < 20; iteration++) // is 20 iterations enough?
	{
	  // Call the training function
		bStopTraining = FALSE;
	  if (!pAvgALN->Train(nMaxEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask)) // average
	  {
		  fprintf(fpProtocol,"Average ALN training failed!\n");
      fflush(fpProtocol);
      exit(0);
	  }
		if (bStopTraining == TRUE)
		{
			fprintf(fpProtocol, "This training stopped because all leaf nodes have stopped changing!\n");
			bStopTraining = FALSE;
			fprintf(fpProtocol, "\nTraining of the average ALN completed at iteration %d \n", iteration);
			fflush(fpProtocol);
			break;
		}
		fprintf(fpProtocol,"\nIteration %d of training average ALN, RMSE = %f\n", iteration, dblTrainErr);
		fflush(fpProtocol);
		/* this may not be useful
		// we don't need to worry about overtraining because there is no noise
		if((double) nNumberLFNs < (double)nOldNumberLFNs * 0.8)
		{
			fprintf(fpProtocol,"Stopping training average ALN, number %d of active LFNs has shrunk too much\n", nNumberLFNs);
			fflush(fpProtocol);
			break;  // if active LFN growth has almost completely stopped, stop iterating
		}
		nOldNumberLFNs = nNumberLFNs; */
	}
	fflush(fpProtocol);
}

void ALNAPI constructDTREE(int nMaxDepth) // routine
{
	// ******************  CONSTRUCT A DTREE FOR THE AVERAGE ALN *******************************
  fprintf(fpProtocol,"\n***** Constructing an ALN decision tree from the average ALN *****\n");
	DTREE* pAvgDTR;

 // create a single-layer average DTREE directly with ConvertDtree and without splitting
	pAvgDTR = pAvgALN->ConvertDtree(nMaxDepth);
  if(pAvgDTR == NULL)
	{
		fprintf(fpProtocol,"No DTREE was generated from the average ALN. Stopping. \n");
    exit(0);
	}
	else
	{
		WriteDtree(szDTREEFileName,pAvgDTR);
		fprintf(fpProtocol,"The DTREE of the average of ALNs %s  was written.\n",szDTREEFileName );
	  fflush(fpProtocol);
  }
}

void ALNAPI cleanup() // routine
{
  if(bTrain)
  {
		// cleanup just what was allocated for training
		for (int n = 0; n < nALNs; n++)
		{
			apALN[n]->Destroy();
		}
		free(apALN);
		pAvgALN->Destroy();
    // the TV file is not created for evaluation
		TVfile.Destroy(); 
    TRfile.Destroy();
    TSfile.Destroy();
    VARfile.Destroy();
    free(adblEpsilon);
	}
  
  // clean up the rest
	PreprocessedDataFile.Destroy();                                       
	free(adblMinVar);
	free(adblMaxVar);
	free(adblStdevVar);
  fclose(fpOutput);
  fclose(fpProtocol);
}

// file task.cpp

void fillvector(double * adblX, CMyAln* paln) // routine
// This is called by the callback in CMyAln to fill in a data vector for training.
// If adblData is set by TRfile.GetDataPtr() and used in paln.SetDataInfo(nPoints,nCols,adblData)
// then the callback does not need to call this routine to get a training vector.
// It uses FillInputVector(...) instead, a routine that can't do the average.
{
	long nRow;
	nRow = (long)floor(ALNRandFloat() * (double) nRowsTR); // This is where the TRfile is indicated for training.
	for(int i = 0; i < nDim; i++)
	{
		adblX[i] = TRfile.GetAt(nRow,i,0); // Notice that TRfile is fixed. Global nRowsTR is not.
		// To get data in real time, you likely have to write new code.
	}
	if(bTrainingAverage) // In this case, we don't use the output component from above. 
	{
		// To average several ALNs, we create new samples around those in TRfile and average the ALN outputs
		// The average ALN is created with negligible smoothing
		const ALNCONSTRAINT* pConstr;
		int nalns = nALNs;
		double dblValue;
		ALNNODE* pActiveLFN;
		for(int i = 0; i < nDim -1; i++) 
		{
			// The idea here is the same as jitter, and gives  much better sampling.
			// The chosen point is triangularly distributed around the initial point
			pConstr = paln->GetConstraint(i, 0);
			ASSERT(pConstr != NULL);
			adblX[i] += (ALNRandFloat() - ALNRandFloat()) * pConstr->dblEpsilon;  // this dblEpsilon is the size of a box "belonging to" a point in the i-th axis
		}
		double sum = 0;
		for (int n = 0; n < nalns; n++)
		{
			dblValue = apALN[n]->QuickEval(adblX, &pActiveLFN);
			sum += dblValue;
		}
		// put the bagging result into the training vector
		adblX[nDim-1] = sum / (double) nalns; // this is the result of averaging ALNs for vector j
	}
}


void ALNAPI createTR_VARfiles(int nChoose) // routine
{
	// The TVfile is all of the PreprocessedDataFile which is not used for testing.
	// This routine is used to set up TRfile and VARfile in various ways.
	// 
	// nChoose = 0: LINEAR_REGRESSION. The TVfile  is copied, in the same order,
	// into TRfile.
	// 
	// nChoose = 1: OVERTRAIN1. Create Noise Variance Samples. This first reorders the TRfile and copies the
	// first half into TRfile1 which is used to overtrain an ALN for use with the rest of TRfile
	// to generate noise variance samples.

	// nChoose = 2: OVERTRAIN2. Then the rest of TRfile is copied into TRfile 2
	// which is used to train an ALN for use with the first half of TRfile to generate noise variance samples. 
	// After this routine, VARfile contains all of the noise variance samples, used
	// in the F-test to decide whether or not to split a piece.
	//
	// nChoose = 3: APPROXIMATION. Approximation uses the TRfile from OVERTRAIN1 and the noise
	// variance samples in VARfile. To do a training which avoids overtraining.
	// 
	// nChoose = 4: BAGGING. For averaging several ALNs, the values of the noise
	// variance samples are divided by the number of ALNs averaged.
	// Again, this avoids overtraining.
	
	double dblValue;
	long i;
	int j;
	if (nChoose == 0) // LINEAR_REGRESSION
	{
		// First we fill the training file TRfile from TVfile.
		nRowsTR = TVfile.RowCount();
		TRfile.Create(nRowsTR, nALNinputs); 
		fprintf(fpProtocol, "TRfile created for linear regression\n");
		fflush(fpProtocol);
		for (i = 0; i < nRowsTV; i++)
		{
			for (j = 0; j < nDim; j++)
			{
				dblValue = TVfile.GetAt(i, j, 0);
				TRfile.SetAt(i, j, dblValue, 0);
			} // end of j loop
		} // end of i loop
		// At this point the TRfile is set up for linear regression
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileLR.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileLR.txt");
	}	// end if(nChoose == 0) LINEAR_REGRESSION

	if (nChoose == 1) // OVERTRAINING1
	{
		nRowsVAR = nRowsTV;
		VARfile.Create(nRowsVAR, nALNinputs); // We fill VARfile first
		long tmp0 = 0; // Index for the next sample going to TRfile1
		long tmp1 = nRowsTR - 1;  // Index for the next sample going to TRfile2
		BOOL bSwitch;
		// We first randomize TRfile by itself.
		for (i = 0; i < nRowsTV; i++)
		{
			bSwitch = (ALNRandFloat() < 0.5) ? FALSE : TRUE; //  Where does  the i-th
																								// row of TVfile go? It goes ...
			if (bSwitch)
			{
				for (j = 0; j < nDim; j++) // ... to the front of TRfile and VARfile, or ...
				{
					dblValue = TVfile.GetAt(i, j, 0);
					TRfile.SetAt(tmp0, j, dblValue, 0);
					VARfile.SetAt(tmp0, j, dblValue, 0);
				}
				tmp0++;
			}
			else
			{
				for (j = 0; j < nDim; j++) // ... to the back.
				{
					dblValue = TVfile.GetAt(i, j, 0);
					TRfile.SetAt(tmp1, j, dblValue, 0);
					VARfile.SetAt(tmp1, j, dblValue, 0);
				}
				tmp1--;
			} //end of if (bSwitch)
		} // end of i loop
		ASSERT(tmp1 == tmp0 - 1); // invariant: tmp1-tmp0 + <rows filled> = nRowsTR - 1
		nRowsSet1 = nRowsTV / 2; // This is how we'll divide the TVfile into 2 almost equal parts.
		TRfile1.Create(nRowsSet1, nALNinputs);
		for (i = 0; i < nRowsSet1; i++)
		{
			for (j = 0; j < nDim; j++) // ... to the back.
			{
				dblValue = TRfile.GetAt(i, j, 0);
				TRfile1.SetAt(tmp1, j, dblValue, 0);
				// Now the part of the VARfile to be compared to the training on TRfile2
				VARfile.SetAt(tmp1, j, dblValue, 0);
			}
		}
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfile1OT.txt");
		// The VARfile is not yet finished.
	} // end (nChoose == 1) // OVERTRAINING1

	if (nChoose == 2) // OVERTRAINING2
	{
		TRfile2.Create(nRowsTV - nRowsSet1, nALNinputs);
		for (i = 0; i < nRowsTR - nRowsSet1; i++)
		{
			for (j = 0; j < nDim; j++) // ... to the back.
			{
				dblValue = TRfile.GetAt(i+nRowsSet1, j, 0);
				TRfile2.SetAt(i, j, dblValue, 0);
				// Now the part of the VARfile to be compared to the training on TRfile1
				VARfile.SetAt(i+nRowsSet1, j, dblValue, 0);
			}
		}
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfile2OT.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileOT.txt");
	} // end (nChoose == 2) // OVERTRAINING2

	if (nChoose == 3) // APPROXIMATION
	{
		ASSERT((nRowsTV == nRowsTR) && (nRowsTV == nRowsVAR));
		TRfile1.Destroy();
		TRfile2.Destroy();
		// We have TRfile and VARfile from previous steps.
		// if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileAP.txt");
		// if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileAP.txt");
	} // This ends if (nChoose == 3) APPROXIMATION

	if (nChoose == 4) // BAGGING
	{
		// Here we again leave the TRfile unchanged but we divide the
		// noise variance values in VARfile by nALNs because of averaging.
		// For the averaging, we must use the fillvector routine, which requires setting
		// two of the parameters to NULL as in SetDataInfo(...,..., NULL, NULL)..
		ASSERT(nRowsVAR == nRowsTV);
		for (i = 0; i < nRowsVAR; i++)
		{
			dblValue = VARfile.GetAt(i, nDim - 1, 0);
			VARfile.SetAt(i, nDim - 1, dblValue/nALNs, 0);
		} // end of i loop
		//if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileBAG.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileBAG.txt");
	} // This ends if (nChoose == 4) BAGGING
} // This ends createTR_VARfiles


void createSamples(int nOTTR, CMyAln* pOTTR)  // routine
{
	ASSERT(pOTTR);
	ALNNODE* pActiveLFN;
	double dblValue, dblALNValue;
	double * adblX = (double *)malloc((nDim) * sizeof(double));
	if (nOTTR == 1)
	{
		for (long i = 0; i < nRowsVAR - nRowsSet1; i++)
		{
			for (int j = 0; j < nDim; j++)
			{
				adblX[j] = VARfile.GetAt(i + nRowsSet1, nDim - 1, 0);
			}
			dblALNValue = pOTTR->QuickEval(adblX, &pActiveLFN);
			VARfile.SetAt(i + nRowsSet1, nDim - 1, pow((adblX[nDim - 1] - dblALNValue), 2) / (1 + 1 / nDim));
		}
	}
	else // nOTTR = 2
	{
		for (long i = 0; i < nRowsSet1; i++)
		{
			for (int j = 0; j < nDim; j++)
			{
				adblX[j] = VARfile.GetAt(i, nDim - 1, 0);
			}
			dblALNValue = pOTTR->QuickEval(adblX, &pActiveLFN);
			VARfile.SetAt(i, nDim - 1, pow((adblX[nDim - 1] - dblALNValue), 2) / (1 + 1 / nDim));
		}
	}
	// Now check to see the global noise variance (You can comment out what follows if it's proven OK)
	// Check a case of known constant noise variance!
	dblValue = 0;
	for(long i = 0; i < nRowsVAR; i++)
	{
		dblValue += VARfile.GetAt(i, nDim - 1, 0);
	}
	fprintf(fpProtocol, "Average of noise variance samples = %f\n", dblValue / nRowsVAR);
	fflush(fpProtocol);
	if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileNV.txt");
	if (bPrint && bDiagnostics) fprintf(fpProtocol, "Diagnose VARfileNV.txt written\n");
}

void prepareQuickStart(CMyAln* pALN)
{
	// We use information from Linear Regression (LR) to save a bit of training time.
	// Set constraints on variables for the ALN
	// NB The following loop excludes the output variable of the ALN, index nDim -1.
	for (int m = 0; m < nDim - 1; m++) 
	{
		pALN->SetEpsilon(adblEpsilon[m], m);
		if (adblEpsilon[m] == 0)
		{
			fprintf(fpProtocol, "Stopping: Variable %d appears to be constant. Try removing it.\n", m);
			fflush(fpProtocol);
			exit(0);
		}
		// The minimum value of the domain is a bit smaller than the min of the data points
		// in TVfile and the maximum is a bit larger.
		pALN->SetMin(adblMinVar[m] - 0.1 * adblStdevVar[m], m);
		pALN->SetMax(adblMaxVar[m] + 0.1 * adblStdevVar[m], m);
		if (!bOvertrain) // In the case of overtraining, we don't want weight constraints!
		{
			// The range of output (for a uniform dist.) divided by the likely distance between samples in axis m.
			pALN->SetWeightMin(-pow(3.0, 0.5) * adblStdevVar[nDim - 1] / adblEpsilon[m], m);
			pALN->SetWeightMax(pow(3.0, 0.5) * adblStdevVar[nDim - 1] / adblEpsilon[m], m); 
			// Impose the a priori bounds on weights.
			if (dblMinWeight[m] > pALN->GetWeightMin(m))
			{
				pALN->SetWeightMin(dblMinWeight[m], m);
			}
			if (dblMaxWeight[m] < pALN->GetWeightMax(m))
			{
				pALN->SetWeightMax(dblMaxWeight[m], m);
			}
		}
	}
	if(bALNgrowable) // This is TRUE for all training except linear regression where these values are created.
	{
		(pALN->GetRegion(0))->dblSmoothEpsilon = adblStdevVar[nDim - 1] / 100.0; // a shot in the dark TEST !!!!
		// use the weights and centroids from linear regression
		ALNNODE* pActiveLFN;
		pActiveLFN = pALN->GetTree();
		for (int m = 0; m < nDim - 1; m++)
		{
			((pActiveLFN)->DATA.LFN.adblC)[m] = adblLRC[m];
			((pActiveLFN)->DATA.LFN.adblW)[m + 1] = adblLRW[m + 1];
		}
		((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] = adblLRC[nDim - 1];
		((pActiveLFN)->DATA.LFN.adblW)[0] = adblLRW[0];
		((pActiveLFN)->DATA.LFN.adblW)[nDim] = -1.0;
	}
}

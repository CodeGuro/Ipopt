// Copyright (C) 2004, International Business Machines and others.
// All Rights Reserved.
// This code is published under the Common Public License.
//
// $Id$
//
// Authors:  Carl Laird, Andreas Waechter     IBM    2004-08-13

#include "IpMa27SymLinearSolver.hpp"
#include "IpDenseVector.hpp"
#include "IpTripletHelper.hpp"

#ifdef OLD_C_HEADERS
# include <math.h>
#else
# include <cmath>
#endif

/** Prototypes for MA27's Fortran subroutines */
extern "C"
{
  void F77_FUNC(ma27id,MA27ID)(ipfint* ICNTL, double* CNTL);
  void F77_FUNC(ma27ad,MA27AD)(ipfint *N, ipfint *NZ, ipfint *IRN, ipfint* ICN,
                               ipfint *IW, ipfint* LIW, ipfint* IKEEP, ipfint *IW1,
                               ipfint* NSTEPS, ipfint* IFLAG, ipfint* ICNTL,
                               double* CNTL, ipfint *INFO, double* OPS);
  void F77_FUNC(ma27bd,MA27BD)(ipfint *N, ipfint *NZ, ipfint *IRN, ipfint* ICN,
                               double* A, ipfint* LA, ipfint* IW, ipfint* LIW,
                               ipfint* IKEEP, ipfint* NSTEPS, ipfint* MAXFRT,
                               ipfint* IW1, ipfint* ICNTL, double* CNTL,
                               ipfint* INFO);
  void F77_FUNC(ma27cd,MA27CD)(ipfint *N, double* A, ipfint* LA, ipfint* IW,
                               ipfint* LIW, double* W, ipfint* MAXFRT,
                               double* RHS, ipfint* IW1, ipfint* NSTEPS,
                               ipfint* ICNTL, double* CNTL);
}

namespace Ipopt
{

  static const Index dbg_verbosity = 0;

  Ma27SymLinearSolver::Ma27SymLinearSolver()
      :
      SymLinearSolver(),
      atag_(0),
      dim_(0),
      nonzeros_(0),
      initialized_(false),
      factorized_(false),

      airn_(NULL),
      ajcn_(NULL),
      liw_(0),
      iw_(NULL),
      ikeep_(NULL),
      la_(0),
      a_(NULL),

      la_increase_(false),
      liw_increase_(false)
  {
    DBG_START_METH("Ma27SymLinearSolver::Ma27SymLinearSolver()",dbg_verbosity);
  }

  Ma27SymLinearSolver::~Ma27SymLinearSolver()
  {
    DBG_START_METH("Ma27SymLinearSolver::~Ma27SymLinearSolver()",
                   dbg_verbosity);
    delete [] airn_;
    delete [] ajcn_;
    delete [] iw_;
    delete [] ikeep_;
    delete [] a_;
  }

  bool Ma27SymLinearSolver::InitializeImpl(const OptionsList& options,
      const std::string& prefix)
  {
    Number value = 0.0;

    if (options.GetNumericValue("pivtol", value, prefix)) {
      ASSERT_EXCEPTION(value>0. && value<1., OptionsList::OPTION_OUT_OF_RANGE,
                       "Option \"pivtol\": This value must be between 0 and 1.");
      pivtol_ = value;
    }
    else {
      pivtol_ = 1e-8;
    }

    if (options.GetNumericValue("pivtolmax", value, prefix)) {
      ASSERT_EXCEPTION(value>=pivtol_ && value<1.,
                       OptionsList::OPTION_OUT_OF_RANGE,
                       "Option \"pivtolmax\": This value must be between pivtol and 1.");
      pivtolmax_ = value;
    }
    else {
      pivtolmax_ = 1e-4;
    }

    if (options.GetNumericValue("liw_init_factor", value, prefix)) {
      ASSERT_EXCEPTION(value>=1., OptionsList::OPTION_OUT_OF_RANGE,
                       "Option \"liw_init_factor\": This value must be at least 1.");
      liw_init_factor_ = value;
    }
    else {
      liw_init_factor_ = 5.;
    }

    if (options.GetNumericValue("la_init_factor", value, prefix)) {
      ASSERT_EXCEPTION(value>=1., OptionsList::OPTION_OUT_OF_RANGE,
                       "Option \"la_init_factor\": This value must be at least 1.");
      la_init_factor_ = value;
    }
    else {
      la_init_factor_ = 5.;
    }

    if (options.GetNumericValue("meminc_factor", value, prefix)) {
      ASSERT_EXCEPTION(value>1., OptionsList::OPTION_OUT_OF_RANGE,
                       "Option \"meminc_factor\": This value must be larger than 1.");
      meminc_factor_ = value;
    }
    else {
      meminc_factor_ = 10.;
    }

    /* Set the default options for MA27 */
    F77_FUNC(ma27id,MA27ID)(icntl_, cntl_);
    cntl_[0] = pivtol_;  // Set pivot tolerance
#ifndef IP_DEBUG

    icntl_[0] = 0;       // Suppress error messages
    icntl_[1] = 0;       // Suppress diagnostic messages
#endif

    // Reset all private data
    atag_=0;
    dim_=0;
    nonzeros_=0;
    initialized_=false;
    factorized_=false;

    la_increase_=false;
    liw_increase_=false;

    return true;
  }

  Ma27SymLinearSolver::ESolveStatus
  Ma27SymLinearSolver::MultiSolve(const SymMatrix& sym_A,
                                  std::vector<const Vector*>& rhsV,
                                  std::vector<Vector*>& solV,
                                  bool check_NegEVals,
                                  Index numberOfNegEVals)
  {
    DBG_START_METH("Ma27SymLinearSolver::MultiSolve",dbg_verbosity);
    DBG_ASSERT(!check_NegEVals || ProvidesInertia());

    // Check if this object has ever seen a matrix If not,
    // allocate memory of the matrix structure and copy the nonzeros
    // structure (it is assumed that this will never change).
    if (!initialized_) {
      InitializeStructure(sym_A);
    }

    DBG_ASSERT(nonzeros_== TripletHelper::GetNumberEntries(sym_A));

    // Perform symbolic manipulations and reserve memory for MA27 data
    // if that hasn't been done before
    if (la_ == 0) {
      // need to do the symbolic manipulations
      ESolveStatus retval;
      retval = SymbolicFactorization();
      if (retval!=S_SUCCESS) {
        return retval;  // Error occurred
      }
    }

    // Check if the matrix data has to be copied into the local data
    // (either it is new, or it has changed)
    DBG_PRINT((1,"atag_=%d sym_A->GetTag()=%d\n",atag_,sym_A.GetTag()))
    if (sym_A.HasChanged(atag_)) {
      factorized_ = false;
      atag_ = sym_A.GetTag();
    }

    // check if a factorization has to be done
    if (!factorized_) {
      // perform the factorization
      ESolveStatus retval;
      retval = Factorization(sym_A, check_NegEVals, numberOfNegEVals);
      if (retval!=S_SUCCESS) {
        DBG_PRINT((1, "FACTORIZATION FAILED!\n"));
        return retval;  // Matrix singular or error occurred
      }
      factorized_ = true;
    }

    // do the backsolve
    return Backsolve(rhsV, solV);
  } // bool Ma27SymLinearSolver::Solve(const Vector& Rhs, Vector &Sol)


  /** Initialize the local copy of the positions of the nonzero
      elements */
  void Ma27SymLinearSolver::InitializeStructure(const SymMatrix& sym_A)
  {
    DBG_START_METH("Ma27SymLinearSolver::InitializeStructure",dbg_verbosity);
    dim_ = sym_A.Dim();
    nonzeros_ = TripletHelper::GetNumberEntries(sym_A);

    delete [] airn_;
    delete [] ajcn_;
    airn_ = new ipfint[nonzeros_];
    ajcn_ = new ipfint[nonzeros_];

    TripletHelper::FillRowCol(nonzeros_, sym_A, airn_, ajcn_);
    if (DBG_VERBOSITY()>=2) {
      for (Index i=0; i<nonzeros_; i++) {
        DBG_PRINT((2, "KKT(%d,%d) = value\n", airn_[i], ajcn_[i]));
      }
    }

    initialized_ = true;
    factorized_ = false;
  }

  Ma27SymLinearSolver::ESolveStatus
  Ma27SymLinearSolver::SymbolicFactorization()
  {
    DBG_START_METH("Ma27SymLinearSolver::SymbolicFactorization",dbg_verbosity);

    // Get memory for the IW workspace
    delete [] iw_;

    // Overstimation factor for LIW (20% recommended in MA27 documentation)
    const double LiwFact = 2.0;   // This is 100% overestimation
    Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                   "In Ma27SymLinearSolver::InitializeStructure: Using overestimation factor LiwFact = %e\n",
                   LiwFact);
    liw_ = (ipfint)(LiwFact*(double(2*nonzeros_+3*dim_+1)));
    iw_ = new ipfint[liw_];

    // Get memory for IKEEP
    delete [] ikeep_;
    ikeep_ = new ipfint[3*dim_];

    // Call MA27AD (cast to ipfint for Index types)
    ipfint N = dim_;
    ipfint NZ = nonzeros_;
    ipfint IFLAG = 0;
    double OPS;
    ipfint INFO[20];
    ipfint* IW1 = new ipfint[2*dim_];  // Get memory for IW1 (only local)
    F77_FUNC(ma27ad,MA27AD)(&N, &NZ, airn_, ajcn_, iw_, &liw_, ikeep_,
                            IW1, &nsteps_, &IFLAG, icntl_, cntl_,
                            INFO, &OPS);
    delete [] IW1;  // No longer required

    // Receive several information
    ipfint iflag = INFO[0];   // Information flag
    ipfint ierror = INFO[1];  // Error flag
    ipfint nrlnec = INFO[4];  // recommended value for la
    ipfint nirnec = INFO[5];  // recommended value for liw

    // Check if error occurred
    if (iflag!=0) {
      Jnlst().Printf(J_ERROR, J_LINEAR_ALGEBRA,
                     "*** Error from MA27AD *** IFLAG = %d IERROR = %d\n", iflag, ierror);
      return S_FATAL_ERROR;
    }

    // ToDo: try and catch
    // Reserve memory for iw_ for later calls, based on suggested size
    delete [] iw_;
    liw_ = (ipfint)(liw_init_factor_ * (double)(nirnec));
    iw_ = new ipfint[liw_];

    // Reserve memory for a_
    delete [] a_;
    la_ = Max(nonzeros_,(ipfint)(la_init_factor_ * (double)(nrlnec)));
    a_ = new double[la_];

    return S_SUCCESS;
  }

  Ma27SymLinearSolver::ESolveStatus
  Ma27SymLinearSolver::Factorization(const SymMatrix& A,
                                     bool check_NegEVals,
                                     Index numberOfNegEVals)
  {
    DBG_START_METH("Ma27SymLinearSolver::Factorization",dbg_verbosity);
    // Check if la should be increased
    if (la_increase_) {
      delete [] a_;
      ipfint la_old = la_;
      la_ = (ipfint)(meminc_factor_ * (double)(la_));
      a_ = new double[la_];
      la_increase_ = false;
      Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                     "In Ma27SymLinearSolver::Factorization: Increasing la from %d to %d\n",
                     la_old, la_);
    }

    // Check if liw should be increased
    if (liw_increase_) {
      delete [] iw_;
      ipfint liw_old = liw_;
      liw_ = (ipfint)(meminc_factor_ * (double)(liw_));
      iw_ = new ipfint[liw_];
      liw_increase_ = false;
      Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                     "In Ma27SymLinearSolver::Factorization: Increasing liw from %d to %d\n",
                     liw_old, liw_);
    }

    ipfint iflag;  // Information flag
    ipfint ncmpbr; // Number of double precision compressions
    ipfint ncmpbi; // Number of integer compressions

    // Call MA27BD; possibly repeatedly if workspaces are too small
    bool done = false;
    while (!done) {
      // Copy Matrix data into a_
      DBG_PRINT_MATRIX(2, "A", A);
      TripletHelper::FillValues(nonzeros_, A, a_);

      ipfint N=dim_;
      ipfint NZ=nonzeros_;
      ipfint* IW1 = new ipfint[2*dim_];
      ipfint INFO[20];

#     ifdef IP_DEBUG

      for (Index i=0; i<NZ; i++) {
        DBG_PRINT((2, "KKT(%d,%d) = %g\n", airn_[i], ajcn_[i], a_[i]));
      }
#     endif
      F77_FUNC(ma27bd,MA27BD)(&N, &NZ, airn_, ajcn_, a_,
                              &la_, iw_, &liw_, ikeep_, &nsteps_,
                              &maxfrt_, IW1, icntl_, cntl_, INFO);
      delete [] IW1;

      // Receive information about the factorization
      iflag = INFO[0];        // Information flag
      ipfint ierror = INFO[1];  // Error flag
      ncmpbr = INFO[11];      // Number of double compressions
      ncmpbi = INFO[12];      // Number of integer compressions
      negevals_ = INFO[14];   // Number of negative eigenvalues

      DBG_PRINT((1,"Return from MA27 iflag = %d and ierror = %d\n",
                 iflag, ierror));

      // Check if factorization failed due to insufficient memory space
      // iflag==-3 if LIW too small (recommended value in ierror)
      // iflag==-4 if LA too small (recommended value in ierror)
      if (iflag==-3 || iflag==-4) {
        // Increase size of both LIW and LA
        delete [] iw_;
        delete [] a_;
        ipfint liw_old = liw_;
        ipfint la_old = la_;
        if(iflag==-3) {
          liw_ = (ipfint)(meminc_factor_ * (double)(ierror));
          la_ = (ipfint)(meminc_factor_ * (double)(la_));
        }
        else {
          liw_ = (ipfint)(meminc_factor_ * (double)(liw_));
          la_ = (ipfint)(meminc_factor_ * (double)(ierror));
        }
        iw_ = new ipfint[liw_];
        a_ = new double[la_];
        Jnlst().Printf(J_WARNING, J_LINEAR_ALGEBRA,
                       "MA27BD returned iflag=%d.\n Increase liw from %d to %d and la from %d to %d and factorize again.\n",
                       iflag, liw_old, liw_, la_old, la_);
        // ToDo: try and catch
      }
      else {
        done = true;
      }
    }

    // Check if the system is singular, and if some other error occurred
    if (iflag==-5 || iflag==3) {
      return S_SINGULAR;
    }
    else if (iflag != 0) {
      // There is some error
      return S_FATAL_ERROR;
    }

    // Check if it might be more efficient to use more memory next time
    // (if there were too many compressions for this factorization)
    if (ncmpbr>=10) {
      la_increase_ = true;
      Jnlst().Printf(J_WARNING, J_LINEAR_ALGEBRA,
                     "MA27BD returned ncmpbr=%d. Increase la before the next factorization.\n",
                     ncmpbr);
    }
    if (ncmpbi>=10) {
      liw_increase_ = true;
      Jnlst().Printf(J_WARNING, J_LINEAR_ALGEBRA,
                     "MA27BD returned ncmpbi=%d. Increase liw before the next factorization.\n",
                     ncmpbr);
    }

    // Check whether the number of negative eigenvalues matches the requested
    // count
    if (check_NegEVals && (numberOfNegEVals!=negevals_)) {
      return S_WRONG_INERTIA;
    }

    return S_SUCCESS;
  }

  Ma27SymLinearSolver::ESolveStatus
  Ma27SymLinearSolver::Backsolve(std::vector<const Vector*>& rhsV,
                                 std::vector<Vector*>& solV)
  {
    DBG_START_METH("Ma27SymLinearSolver::Backsolve",dbg_verbosity);
    // Determine number of right hand sides
    Index nrhs = (Index)rhsV.size();
    DBG_ASSERT(nrhs==solV.size());

    // For each right hand side, call MA27CD
    for(Index i=0; i<nrhs; i++) {
      DBG_ASSERT(rhsV[i]);
      DBG_ASSERT(rhsV[i]->Dim() == dim_);
      double* sol_vals = new double[dim_];
      TripletHelper::FillValuesFromVector(dim_, *rhsV[i], sol_vals);

      ipfint N=dim_;
      double* W = new double[maxfrt_];
      ipfint* IW1 = new ipfint[nsteps_];
      F77_FUNC(ma27cd,MA27CD)(&N, a_, &la_, iw_, &liw_, W, &maxfrt_,
                              sol_vals, IW1, &nsteps_,
                              icntl_, cntl_);

      // Put the solution values back into the vector
      TripletHelper::PutValuesInVector(dim_, sol_vals, *solV[i]);

      delete [] W;
      delete [] IW1;
      delete [] sol_vals;
    }

    return S_SUCCESS;
  }

  Index Ma27SymLinearSolver::NumberOfNegEVals() const
  {
    DBG_START_METH("Ma27SymLinearSolver::NumberOfNegEVals",dbg_verbosity);
    DBG_ASSERT(factorized_);
    DBG_ASSERT(ProvidesInertia());
    return negevals_;
  }

  bool Ma27SymLinearSolver::IncreaseQuality()
  {
    DBG_START_METH("Ma27SymLinearSolver::IncreaseQuality",dbg_verbosity);
    if (pivtol_ == pivtolmax_) {
      return false;
    }
    factorized_ = false;
    pivtol_ = Min(pivtolmax_, pow(pivtol_,0.75));
    return true;
  }

} // namespace Ipopt

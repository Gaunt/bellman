//
// Copyright (c) 2011 Ronaldo Carpio
//                                     
// Permission to use, copy, modify, distribute and sell this software
// and its documentation for any purpose is hereby granted without fee,
// provided that the above copyright notice appear in all copies and   
// that both that copyright notice and this permission notice appear
// in supporting documentation.  The authors make no representations
// about the suitability of this software for any purpose.          
// It is provided "as is" without express or implied warranty.
//                                                            
  


#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <float.h>

#include <string>
#include <vector>

#include <Python.h>
#include <numpy/arrayobject.h>

#include "tbb/parallel_reduce.h"
#include "tbb/blocked_range2d.h"

// multi-threaded library
using namespace tbb;


int getDoublesFromPySequence(PyObject *sequence, DoubleArray &dest) {
  int i, n;
  long total = 0;
  PyObject *item;
  double x;
  
  n = PySequence_Length(sequence);
  if (n < 0)
      return -1; /* Has no length */
  dest.resize(n);
  for (i = 0; i < n; i++) {
    item = PySequence_GetItem(sequence, i);
    if (item == NULL)
      return -1; /* Not a sequence, or other failure */
	if (PyArg_Parse(item, "d", &x)) {
      dest[i] = x;
	  total++;
	}
    Py_DECREF(item); /* Discard reference ownership */
  }
  return total;
}

int getPyArrayFromPySequence(PyObject *sequence, PyArrayObjectPtr_array &dest) {
  int i, n;
  long total = 0;
  PyObject *item;
  n = PySequence_Length(sequence);
  if (n < 0)
      return -1; /* Has no length */
  dest.resize(n);
  for (i = 0; i < n; i++) {
    item = PySequence_GetItem(sequence, i);
    if (item == NULL)
      return -1; /* Not a sequence, or other failure */
    if (PyArray_Check(item)) {
      dest[i] = (PyArrayObject*) item;
	  total += 1;
	}
    Py_DECREF(item); /* Discard reference ownership */
  }
  return total;
}
	  
// return the cell that contains value.
// if below grid min, return -1
// if above grid max, return len(pGrid)-2
int getCellIndex(double value, PyArrayObject *pGrid) {
  double dx = *ARRAYPTR1D(pGrid, 1) - *ARRAYPTR1D(pGrid, 0);
  if (value < *ARRAYPTR1D(pGrid, 0)) {
    return -1;
  } else if (value >= *ARRAYPTR1D(pGrid, ARRAYLEN1D(pGrid)-1)) {
    return ARRAYLEN1D(pGrid) - 2;
  } else {
    int result = floor((value - *ARRAYPTR1D(pGrid, 0)) / dx);
	if (result == (ARRAYLEN1D(pGrid) - 1)) {
	  result--;
	}
	return result;
  }
}

// if x is outside grid, force it to boundaries
double forceToGrid(double x, PyArrayObject *pGrid) {
  double first = *ARRAYPTR1D(pGrid, 0);
  if (x < first) {
    return first;
  }
//  int len = ARRAYLEN1D(pGrid);
//  double last = *ARRAYPTR1D(pGrid, len - 1);
  double last = *ARRAYPTR1D(pGrid, ARRAYLEN1D(pGrid)-1);
  if (x > last) {
    return last;
  }
  return x;
}

// 1d interpolation between 2 points, x1,x2 with function values f1 = f(x1), f2 = f(x2)
double interp1d(double z, double x1, double f1, double x2, double f2) {
  if (z <= x1) {
    return f1;
  }
  if (z >= x2) {
    return f2;
  }
  double result = f1 + (f2-f1)*(z-x1)/(x2-x1);
  return result;
}

// bilinear interpolation between 4 points of a rectangle, corners (x1,y1), (x2,y2)
// f_1_1 = f(x1,y1)
double interp2d(double z1, double z2, double x1, double y1, double x2, double y2, double f_1_1, double f_1_2, double f_2_1, double f_2_2) {  
  // interp1d along y1 line, then along y2 line
  double f_y1 = interp1d(z1, x1, f_1_1, x2, f_2_1);
  double f_y2 = interp1d(z1, x1, f_1_2, x2, f_2_2);
  // then, interp1d again
  double result = interp1d(z2, y1, f_y1, y2, f_y2);
  return result;
}

// bilinear interploation given the grid
double interp2d_grid(PyArrayObject *pGrid1, PyArrayObject *pGrid2, PyArrayObject *pF, double xi, double yi) {
  double a,b;
  int i,j;
  double x1,x2,y1,y2,u1,u2,u3,u4;
  // figure out which cell xi,yi is in
  a = forceToGrid(xi, pGrid1);
  b = forceToGrid(yi, pGrid2);
  i = getCellIndex(a, pGrid1);
  j = getCellIndex(b, pGrid2);
  // get values of x,y,f at corners
  x1 = *ARRAYPTR1D(pGrid1, i);
  x2 = *ARRAYPTR1D(pGrid1, i+1);
  y1 = *ARRAYPTR1D(pGrid2, j);
  y2 = *ARRAYPTR1D(pGrid2, j+1);
  u1 = *ARRAYPTR2D(pF, i, j);
  u2 = *ARRAYPTR2D(pF, i+1, j);
  u3 = *ARRAYPTR2D(pF, i, j+1);
  u4 = *ARRAYPTR2D(pF, i+1, j+1);
  // interpolate
  double result = interp2d(xi, yi, x1, y1, x2, y2, u1, u3, u2, u4);
  return result;
}
  
// trilinear interpolation
// pF is a 3d array of doubles
// pGrid1-3 are 1d arrays with the grid coords (must be evenly spaced)
// return interpolated value f(x1, x2, x3)
double interpTrilinear(PyArrayObject *pGrid1, PyArrayObject *pGrid2, PyArrayObject *pGrid3, PyArrayObject *pF,
    double xi, double yi, double zi) {
  double a, b, c;
  int i, j, k;
  double x1, x2, y1, y2, z1, z2;
  double u1, u2, u3, u4, u5, u6, u7, u8;
  double w1, w2, w3, w4, w5, w6, w7, u;

  a = forceToGrid(xi, pGrid1);
  b = forceToGrid(yi, pGrid2);
  c = forceToGrid(zi, pGrid3);
  i = getCellIndex(a, pGrid1);
  j = getCellIndex(b, pGrid2);
  k = getCellIndex(c, pGrid3);
  
  x1 = *ARRAYPTR1D(pGrid1, i);
  x2 = *ARRAYPTR1D(pGrid1, i+1);
  y1 = *ARRAYPTR1D(pGrid2, j);
  y2 = *ARRAYPTR1D(pGrid2, j+1);
  z1 = *ARRAYPTR1D(pGrid3, k);
  z2 = *ARRAYPTR1D(pGrid3, k+1);
  
  u1 = *ARRAYPTR3D(pF, i, j, k);
  u2 = *ARRAYPTR3D(pF, i+1, j, k);
  u3 = *ARRAYPTR3D(pF, i, j+1, k);
  u4 = *ARRAYPTR3D(pF, i+1, j+1, k);
  u5 = *ARRAYPTR3D(pF, i, j, k+1);
  u6 = *ARRAYPTR3D(pF, i+1, j, k+1);
  u7 = *ARRAYPTR3D(pF, i, j+1, k+1);
  u8 = *ARRAYPTR3D(pF, i+1, j+1, k+1);

  w1 = u2 + (u2-u1)/(x2-x1)*(a-x2);
  w2 = u4 + (u4-u3)/(x2-x1)*(a-x2);
  w3 = w2 + (w2-w1)/(y2-y1)*(b-y2);
  w4 = u5 + (u6-u5)/(x2-x1)*(a-x1);
  w5 = u7 + (u8-u7)/(x2-x1)*(a-x1);
  w6 = w4 + (w5-w4)/(y2-y1)*(b-y1);
  w7 = w3 + (w6-w3)/(z2-z1)*(c-z1);
  u = w7;

  return u;
}

// test trilinear interpolation
double f3(double x, double y, double z) {
  return x + y + z;
}

PyObject *testInterp2(PyObject *self, PyObject *args) {
  double x1, x2, f;
  PyArrayObject *pGrid1=NULL, *pGrid2=NULL, *pF=NULL;

  if (!PyArg_ParseTuple(args, "O!O!O!dd:testInterp2", &PyArray_Type, &pGrid1, &PyArray_Type, &pGrid2, 
      &PyArray_Type, &pF,
      &x1, &x2)) {
    return NULL;
  }
  f = interp2d_grid(pGrid1, pGrid2, pF, x1, x2);
  return Py_BuildValue("d", f);
}

PyObject *testInterp3(PyObject *self, PyObject *args) {
  double x1, x2, x3, f;
  PyArrayObject *pGrid1=NULL, *pGrid2, *pGrid3, *pF;

  if (!PyArg_ParseTuple(args, "O!O!O!O!ddd:testInterp3", &PyArray_Type, &pGrid1, &PyArray_Type, &pGrid2, 
      &PyArray_Type, &pGrid3, &PyArray_Type, &pF,
      &x1, &x2, &x3)) {
    return NULL;
  }
  f = interpTrilinear(pGrid1, pGrid2, pGrid3, pF, x1, x2, x3);
  return Py_BuildValue("d", f);
}

// 2d grid search.  returns # of values found
int gridSearch2D(PyArrayObject *pGrid1, PyArrayObject *pGrid2, ddFn2* pFn, void* pArgs, double *pMaxVal, double *pArgmax1, double *pArgmax2) {
  assert(pGrid1->nd == pGrid2->nd &&  pGrid1->nd == 1); 
  
  //(pA)->data + (i)*(pA)->strides[0]
  char *pData1 = NULL;
  char *pData2 = NULL;
  int len1 = pGrid1->dimensions[0];
  int len2 = pGrid2->dimensions[0];
  int stride1 = pGrid1->strides[0];
  int stride2 = pGrid2->strides[0];  
  double result, max, arg1, arg2;
  int i=0, count=0;
  char *pLast1 = (pGrid1->data) + (stride1 * len1);
  char *pLast2 = (pGrid2->data) + (stride2 * len2);
  for (pData1=pGrid1->data; pData1 != pLast1; pData1 += stride1) {
    for (pData2=pGrid2->data; pData2 != pLast2; pData2 += stride2) {   
      arg1 = * (double*) pData1;
	  arg2 = * (double*) pData2;
      result = (*pFn)(arg1, arg2, pArgs);
	  if (i == 0) {
        max = result;
	    *pArgmax1 = arg1;
	    *pArgmax2 = arg2;
	    count = 1;
      } else {
        if (result > max) {
          max = result;
  	      *pArgmax1 = arg1;
	      *pArgmax2 = arg2;		
		  count = 1;
	    } else if (result == max) {
	      count++;
	    }
	  }
	  i++;
	}
  }
  *pMaxVal = max;
  return count;
}

// could be the ties among equals.
// parallel version
class MaxIndexFnObj {  
public:
  double m_value_of_max;
  double m_argmax1, m_argmax2;
  ddFn2* m_pFn;
  PyArrayObject *m_pGrid1, *m_pGrid2;
  int m_len1;
  int m_len2;
  int m_stride1;
  int m_stride2;
  void *m_pArgs;
  
  void operator()( const blocked_range2d<size_t, size_t>& r ) {   
    char *pData1 = NULL;
    char *pData2 = NULL;  
	double value, arg1, arg2;
    // r is a 2-dimensonal range
    for( size_t i=r.rows().begin(); i!=r.rows().end(); ++i ){
	  pData1 = (m_pGrid1->data) + (m_stride1 * i);
      for( size_t j=r.cols().begin(); j!=r.cols().end(); ++j) {        
        pData2 = (m_pGrid2->data) + (m_stride2 * j);	  
        arg1 = * (double*) pData1;
	    arg2 = * (double*) pData2;      
        value = (*m_pFn)(arg1, arg2, m_pArgs);	  
        if (m_value_of_max == -DBL_MAX || value > m_value_of_max) {
          m_value_of_max = value;
          m_argmax1 = arg1;
		  m_argmax2 = arg2;
		}
      }
    }
  }
  MaxIndexFnObj( MaxIndexFnObj& x, split ) :  // split constructor from tbb    
    m_value_of_max(-DBL_MAX), // -DBL_MAX from <climits>
    m_argmax1(-DBL_MAX), m_argmax2(-DBL_MAX),
	m_pFn(x.m_pFn), m_pGrid1(x.m_pGrid1), m_pGrid2(x.m_pGrid2), m_len1(x.m_len1), m_len2(x.m_len2), m_stride1(x.m_stride1), m_stride2(x.m_stride2),
	m_pArgs(x.m_pArgs)
  {
  }
  // this is where results from different threads will be compared
  void join( const MaxIndexFnObj& y ) {
    if (m_value_of_max == -DBL_MAX || y.m_value_of_max > m_value_of_max) {
      m_value_of_max = y.m_value_of_max;
	  m_argmax1 = y.m_argmax1;
	  m_argmax2 = y.m_argmax2;
    }
  }  
  MaxIndexFnObj(PyArrayObject *pGrid1, PyArrayObject *pGrid2, ddFn2* pFn, void* pArgs) :    
    m_value_of_max(-DBL_MAX), // -DBL_MAX from <climits>
    m_argmax1(-DBL_MAX), m_argmax2(-DBL_MAX)
  {
    //(pA)->data + (i)*(pA)->strides[0]
	m_pFn = pFn;
	m_pArgs = pArgs;
	m_pGrid1 = pGrid1;
	m_pGrid2 = pGrid2;
    m_len1 = pGrid1->dimensions[0];
    m_len2 = pGrid2->dimensions[0];
    m_stride1 = pGrid1->strides[0];
    m_stride2 = pGrid2->strides[0];  
  }
};

// wrap a gridArray with something that maps indices to a 1d range (obviously total size can't exceed the max value of an int)
unsigned int IndexListTo1D(IntArray const &lenArray, IntArray const &indexArray) {
  int i;  
  assert(lenArray.size() == indexArray.size());
  if (lenArray.size() == 0) {
    return 0;
  }
  unsigned int result = indexArray[0];
  for (i=1; i<lenArray.size(); i++) {
    result *= lenArray[i];
    result += indexArray[i];
  }
  return result;
}

void Index1DToArray(unsigned int index, IntArray const &lenArray, IntArray &dest) {
  int i;
  dest.resize(lenArray.size());
  for (i=lenArray.size()-1; i>=0; i--) {
    dest[i] = index % lenArray[i];
	index /= lenArray[i];
  }
  assert(index == 0);
  return;
}

PyObject *testIndex(PyObject *self, PyObject *args) {
  unsigned int i, j;
  int x1, x2;
  IntArray indexArray(2), lenArray(2);
  
  if (!PyArg_ParseTuple(args, "ii:testf", &x1, &x2)) { 
    return NULL;
  }
  lenArray[0] = x1;
  lenArray[1] = x2;
  for (i=0; i<x1*x2; i++) {
    Index1DToArray(i, lenArray, indexArray);
	j = IndexListTo1D(lenArray, indexArray);
	if (j != i) {
	   return Py_BuildValue("i", j);
    }
  }
  return Py_BuildValue("i", -1);
}

class MaxIndexFnObj2 {  
public:
  double m_value_of_max;
  DoubleArray m_argmax;
  DoubleArrayFn *m_pFn;
  PyArrayObjectPtr_array m_gridPtrArray;
  IntArray m_lenArray;
  void *m_pArgs;
  
  void operator()( const blocked_range<size_t>& r ) {   
    char *pData = NULL;    
	double value, arg;
	int j;
	IntArray indexArray(m_lenArray.size());
	DoubleArray argArray(m_lenArray.size());
    
    for( size_t index=r.begin(); index!=r.end(); ++index ){
	  Index1DToArray(index, m_lenArray, indexArray);
	  for (j=0; j<indexArray.size(); j++) {
	    pData = (m_gridPtrArray[j]->data) + (m_gridPtrArray[j]->strides[0] * indexArray[j]);
		arg = * (double*) pData;
		argArray[j] = arg;
	  }
      value = (*m_pFn)(argArray, m_pArgs);	  
      if (value > m_value_of_max) {
        m_value_of_max = value;
		m_argmax = argArray;
      }
    }
  }
  MaxIndexFnObj2( MaxIndexFnObj2& x, split ) :  // split constructor from tbb    
    m_value_of_max(-DBL_MAX), // -DBL_MAX from <climits>
    m_argmax(x.m_argmax),
	m_pFn(x.m_pFn), m_gridPtrArray(x.m_gridPtrArray), m_lenArray(x.m_lenArray),
	m_pArgs(x.m_pArgs)
  {
  }
  // this is where results from different threads will be compared
  void join( const MaxIndexFnObj2& y ) {
    if (y.m_value_of_max > m_value_of_max) {
      m_value_of_max = y.m_value_of_max;
	  m_argmax = y.m_argmax;	  
    }
  }  
  MaxIndexFnObj2(PyArrayObjectPtr_array const &gridPtrArray, DoubleArrayFn* pFn, void* pArgs) :    
    m_value_of_max(-DBL_MAX), // -DBL_MAX from <climits>
    m_argmax(gridPtrArray.size(), -DBL_MAX), m_lenArray(gridPtrArray.size())	
  {
    //(pA)->data + (i)*(pA)->strides[0]
	m_pFn = pFn;
	m_pArgs = pArgs;
	m_gridPtrArray = gridPtrArray;
    for (int i=0; i<gridPtrArray.size(); i++) {
	  m_lenArray[i] = gridPtrArray[i]->dimensions[0];
	}
  }
};

int gridSearch2DParallel(PyArrayObject *pGrid1, PyArrayObject *pGrid2, ddFn2* pFn, void* pArgs, double *pMaxVal, double *pArgmax1, double *pArgmax2) {
  assert(pGrid1->nd == pGrid2->nd &&  pGrid1->nd == 1); 
  
  MaxIndexFnObj fnObj(pGrid1, pGrid2, pFn, pArgs);
  int len1 = pGrid1->dimensions[0];
  int len2 = pGrid2->dimensions[0];  
  parallel_reduce( blocked_range2d<size_t>(0, len1, 16, 0, len2, 16), fnObj);  

  *pMaxVal = fnObj.m_value_of_max;
  *pArgmax1 = fnObj.m_argmax1;
  *pArgmax2 = fnObj.m_argmax2;
  return 1;
}

// parallel version with arbitrary dimensions

int gridSearchParallel(PyArrayObjectPtr_array const &gridArray, DoubleArrayFn *pFn, void *pArgs, double *pMaxVal, DoubleArray &argMaxArray) {  
  int nGrids = gridArray.size();
  int i;
  IntArray lenArray(nGrids);
  
  for (i=0; i<nGrids; i++) {
    // check that all grids are 1-dimensional
    assert(gridArray[i]->nd == 1);  
	lenArray[i] = gridArray[i]->dimensions[0];
  }

  unsigned int totalGridSize = 1;
  double totalGridSize2 = 1.0;
  for (i=0; i<nGrids; i++) {
	totalGridSize *= lenArray[i];
	totalGridSize2 *= double(lenArray[i]);
  }
  // check that total grid space isn't too large to be indexed
  assert(totalGridSize2 < double(UINT_MAX));
  
  MaxIndexFnObj2 fnObj(gridArray, pFn, pArgs);  
  parallel_reduce( blocked_range<size_t>(0, totalGridSize, 32), fnObj);  

  *pMaxVal = fnObj.m_value_of_max;
  argMaxArray = fnObj.m_argmax;
  return 1;
}
  
// grid search with an arbitrary number of dimensions
int gridSearch(PyArrayObjectPtr_array const &gridArray, DoubleArrayFn *pFn, void *pArgs, double *pMaxVal, DoubleArray &argMaxArray) {  
  int nGrids = gridArray.size();
  int i;
  IntArray lenArray(nGrids), strideArray(nGrids), dataIndexArray(nGrids);
  
  for (i=0; i<nGrids; i++) {
    // check that all grids are 1-dimensional
    assert(gridArray[i]->nd == 1);
  
	lenArray[i] = gridArray[i]->dimensions[0];
	strideArray[i] = gridArray[i]->strides[0];
	dataIndexArray[i] = 0;
  }

  DoubleArray argArray(nGrids);  
  int nIter = 0;
  int nMaxMultiplicity = 0;  
  bool bDone = false;
  
  // check for zero size, if one grid has zero size then there's nothing to do
  for (i=0; i<nGrids; i++) {
    if (lenArray[i] == 0) {
	  bDone = true;
	  break;
	}
  }
  double max = -DBL_MAX;
  while (!bDone) {
    double result;
	// get double array args from char* data
	for (i=0; i<nGrids; i++) {
	  char *pData = (gridArray[i])->data;
	  char *pArg = pData + (strideArray[i] * dataIndexArray[i]);
	  argArray[i] = *(double*) pArg;
	}
	result = (*pFn)(argArray, pArgs);
	if (nIter == 0) {			// first iteration
      max = result;
	  argMaxArray = argArray;
	  nMaxMultiplicity = 1;
    } else {
      if (result > max) {
        max = result;
		argMaxArray = argArray;
	    nMaxMultiplicity = 1;
	  } else if (result == max) {
	    nMaxMultiplicity++;
	  }
	}
	nIter++;

    // increment indices for next iteration
	bDone = true;
	for (i=nGrids-1; i>=0; i--) {
	  // increment index i
	  dataIndexArray[i] += 1;
	  if (dataIndexArray[i] == lenArray[i]) {
	    // cycle this index
		dataIndexArray[i] = 0;
	  } else {
	    bDone = false;
		break;
	  }
	  // if we make it here, all indices have cycled, therefore we are done
	}
	
  }
  *pMaxVal = max;
  return nMaxMultiplicity;
}


PyObject *maximizer2d(PyObject *self, PyObject *args) {
  double M, D;
  PyArrayObject *pGrid_d = NULL, *pGrid_r = NULL, *pW=NULL;
  int useC, bPrint, bParallel;
  int i;
  
  if (!PyArg_ParseTuple(args, "O!O!O!ddiii:maximizer2d", 
      &PyArray_Type, &pGrid_d, &PyArray_Type, &pGrid_r, &PyArray_Type, &pW, &M, &D, &useC, &bParallel, &bPrint)) {
    return NULL;
  }

  if (pW->nd != 2 ||
      pW->dimensions[0] != g_Params.pGrid1->dimensions[0] ||
      pW->dimensions[1] != g_Params.pGrid2->dimensions[0]) {
    PyErr_SetString(PyExc_ValueError, "w dimensions don't match grid");
    return NULL;
  }
  
  eu_params params(M, D, pW, bPrint);
  double argmax1, argmax2, maxval;
  int count = 0;
  if (useC != 0) {
	if (!bParallel) {
      count = gridSearch2D(pGrid_d, pGrid_r, &calc_exp_util, (void*) &params, &maxval, &argmax1, &argmax2);	
	} else {	  
	  count = gridSearch2DParallel(pGrid_d, pGrid_r, &calc_exp_util, (void*) &params, &maxval, &argmax1, &argmax2);	
	}
  } else {
    // use c++ version
    DoubleArray argmaxArray(2);  
    PyArrayObjectPtr_array gridArray(2);
    gridArray[0] = pGrid_d;
    gridArray[1] = pGrid_r;    
	
    count = gridSearch(gridArray, &calc_exp_util2, (void*) &params, &maxval, argmaxArray);
    argmax1 = argmaxArray[0];
    argmax2 = argmaxArray[1];

	// printf("result: maxval=%f argmax_d=%f argmax_r=%f\n", maxval, argmax1, argmax2);
  }
  return Py_BuildValue("iddd", count, argmax1, argmax2, maxval);
}

// grid search maximizer that takes an arbitrary dimension of state vars, control vars
// 3 args: gridList, argList, w.f
// gridList is a sequence of grids for control vars
// argList is a sequence of doubles, for the state vars
// w.f is a multi-dimensional array
PyObject *maximizer(PyObject *self, PyObject *args) {
  PyObject *pGridList = NULL, *pStateVarList = NULL;
  PyArrayObject *pW = NULL;
  int i;
  int bPrint = 0, bParallel=0;
  
  if (!PyArg_ParseTuple(args, "OOO!ii:maximizer", 
      &pGridList, &pStateVarList, &PyArray_Type, &pW, &bParallel, &bPrint)) {
    return NULL;
  }

  // check that w dimensions match the state var grids in g_Params
  if (pW->nd != g_Params.gridPtrArray.size()) {
    PyErr_SetString(PyExc_ValueError, "w dimensions don't match grid");
    return NULL;
  }
  for (i=0; i<g_Params.gridPtrArray.size(); i++) {
    if (pW->dimensions[i] != g_Params.gridPtrArray[i]->dimensions[0]) {
	  PyErr_SetString(PyExc_ValueError, "w dimensions don't match grid");
	  return NULL;
	}
  }

  // the void* arg will take the state variables and pW    
  eu_params params(pStateVarList, pW, bPrint);
  double maxval;
  int count = 0;
  int nControls = PySequence_Length(pGridList);
  DoubleArray argmaxArray(nControls);  
  PyArrayObjectPtr_array gridArray(nControls);
  if (getPyArrayFromPySequence(pGridList, gridArray) <= 0) {
    PyErr_SetString(PyExc_ValueError, "error in reading grid list");
	return NULL;
  }  
  if (!bParallel) {
    count = gridSearch(gridArray, &calc_exp_util2, (void*) &params, &maxval, argmaxArray);
  } else {
    count = gridSearchParallel(gridArray, &calc_exp_util2, (void*) &params, &maxval, argmaxArray);
  }
  // printf("result: maxval=%f ", maxval);
  // copy argmax values to a python tuple
  PyObject *t = PyTuple_New(nControls);
  for (i=0; i<nControls; i++) {   
    PyTuple_SetItem(t, i, PyFloat_FromDouble(argmaxArray[i]));
	// printf("argmax%d=%f ", i, argmaxArray[i]);
  }
  // printf("\n");
  return Py_BuildValue("idN", count, maxval, t);
}

PyObject *test1(PyObject *self, PyObject *args) {
  int arg1, arg2;
  int bPrint = 0;
  if (!PyArg_ParseTuple(args,  "ii|i:test1", &arg1, &arg2, &bPrint)) {
    return NULL;
  }

  return Py_BuildValue("iii", arg1, arg2, bPrint);
}

PyMethodDef methods[] = {
  {"interp3", testInterp3, METH_VARARGS},
  {"interp2", testInterp2, METH_VARARGS},
  {"setGlobalParams", setGlobalParams, METH_VARARGS},
  {"setOutputFile", setOutputFile, METH_VARARGS},
  {"expUtil", expUtil, METH_VARARGS},
  {"test1", test1, METH_VARARGS},
  {"testf", testf, METH_VARARGS},
  {"maximizer2d", maximizer2d, METH_VARARGS},
  {"maximizer", maximizer, METH_VARARGS},
  {"testIndex", testIndex, METH_VARARGS},
  {NULL, NULL},                    
};             
  
#ifdef __cplusplus
extern "C" {      
#endif      
      
void initponzi3_fns() {
  import_array();
  initGlobalParams();
  (void)Py_InitModule("ponzi3_fns", methods);
}                                      
 
#ifdef __cplusplus
}                 
#endif


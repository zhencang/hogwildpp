// Copyright 2012 Victor Bittorf, Chris Re
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Hogwild!, part of the Hazy Project
// Author : Victor Bittorf (bittorf [at] cs.wisc.edu)
// Original Hogwild! Author: Chris Re (chrisre [at] cs.wisc.edu)             
#include <cstdlib>
#include <numa.h>

#include "hazy/hogwild/hogwild-inl.h"
#include "hazy/hogwild/numa_memory_scan.h"
#include "hazy/scan/tsvfscan.h"
#include "hazy/scan/binfscan.h"

#include "frontend_util.h"

#include "numasvm/svmmodel.h"
#include "svm/svm_loader.h"
#include "numasvm/svm_exec.h"


// Hazy imports
using namespace hazy;
using namespace hazy::hogwild;
using scan::TSVFileScanner;
using scan::MatlabTSVFileScanner;

using hazy::hogwild::svm::fp_type;


using namespace hazy::hogwild::svm;


template <class Scan>
size_t NumaLoadSVMExamples(Scan &scan, vector::FVector<SVMExample> * nodeex, unsigned nnodes) { 
  size_t nfeats = 0;
  for (unsigned i = 0; i < nnodes; ++i) {
    scan.Reset();
    numa_run_on_node(i);
    numa_set_preferred(i);
    nfeats = LoadSVMExamples<Scan>(scan, nodeex[i]);
  }
  numa_run_on_node(-1);
  numa_set_preferred(-1);
  return nfeats;
}

void CreateNumaSVMModel(NumaSVMModel * node_m, size_t nfeats, unsigned nnodes, unsigned nthreads) {
  numa_run_on_node(0);
  numa_set_preferred(0);
  int * atomic_ptr = new int;
  int atomic_mask = (1 << (sizeof(int) * 8 - __builtin_clz(nnodes))) - 1;
  printf("Model array allocated at %p\n", node_m);
  for (unsigned i = 0; i < nnodes; ++i) {
    numa_run_on_node(i);
    numa_set_preferred(i);
    printf("Allocating memory for core %d\n", i);
    node_m[i].AllocateModel(nfeats);
    node_m[i].atomic_ptr = atomic_ptr;
    node_m[i].atomic_mask = atomic_mask;
    if (i == nnodes - 1) {
      node_m[i].atomic_inc_value = atomic_mask - nthreads + 1;
    }
    else {
      node_m[i].atomic_inc_value = 1;
    }
  }
  numa_run_on_node(-1);
  numa_set_preferred(-1);
}

int main(int argc, char** argv) {
  hazy::util::Clock wall_clock;
  wall_clock.Start();
  //Benchmark::StartExperiment(argc, argv);

  bool matlab_tsv = false;
  bool loadBinary = false;
  unsigned nepochs = 20;
  unsigned nthreads = 1;
  float mu = 1.0, step_size = 5e-2, step_decay = 0.8;
  static struct extended_option long_options[] = {
    {"mu", required_argument, NULL, 'u', "the maxnorm"},
    {"epochs"    ,required_argument, NULL, 'e', "number of epochs (default is 20)"},
    {"stepinitial",required_argument, NULL, 'i', "intial stepsize (default is 5e-2)"},
    {"step_decay",required_argument, NULL, 'd', "stepsize decay per epoch (default is 0.8)"},
    {"seed", required_argument, NULL, 's', "random seed (o.w. selected by time, 0 is reserved)"},
    {"splits", required_argument, NULL, 'r', "number of threads (default is 1)"},
    //{"shufflers", required_argument, NULL, 'q', "number of shufflers"},
    {"binary", required_argument,NULL, 'v', "load the file in a binary fashion"},
    {"matlab-tsv", required_argument,NULL, 'm', "load TSVs indexing from 1 instead of 0"},
    {NULL,0,NULL,0,0} 
  };

  char usage_str[] = "<train file> <test file>";
  int c = 0, option_index = 0;
  option* opt_struct = convert_extended_options(long_options);
  while( (c = getopt_long(argc, argv, "", opt_struct, &option_index)) != -1) 
  {
    switch (c) { 
      case 'v':
        loadBinary = (atoi(optarg) != 0);
        break;
      case 'm':
        matlab_tsv = (atoi(optarg) != 0);
        break;
      case 'u':
        mu = atof(optarg);
        break;
      case 'e':
        nepochs = atoi(optarg);
        break;
      case 'i':
        step_size = atof(optarg);
        break;
      case 'd':
        step_decay = atof(optarg);
        break;
      case 'r':
        nthreads = atoi(optarg);
        break;
      case ':':
      case '?':
        print_usage(long_options, argv[0], usage_str);
        exit(-1);
        break;
    }
  }
  SVMParams tp (step_size, step_decay, mu);

  char * szTestFile, *szExampleFile;
  
  if(optind == argc - 2) {
    szExampleFile = argv[optind];
    szTestFile  = argv[optind+1];
  } else {
    print_usage(long_options, argv[0], usage_str);
    exit(-1);
  }
  //fp_type buf[50];

  // we initialize thread pool here because we need CPU topology information
  hazy::thread::ThreadPool tpool(nthreads);
  tpool.Init();
  unsigned nnodes = tpool.NodeCount();
  
  vector::FVector<SVMExample> * node_train_examps = new vector::FVector<SVMExample>[nnodes];
  vector::FVector<SVMExample> * node_test_examps = new vector::FVector<SVMExample>[nnodes];

  size_t nfeats;

  if (loadBinary) {
    printf("Loading binary file...\n");
    scan::BinaryFileScanner scan(szExampleFile);
    nfeats = NumaLoadSVMExamples(scan, node_train_examps, nnodes);
    printf("Loaded binary file!\n");
  } else if (matlab_tsv) {
    MatlabTSVFileScanner scan(szExampleFile);
    nfeats = NumaLoadSVMExamples(scan, node_train_examps, nnodes);
  } else {
    TSVFileScanner scan(szExampleFile);
    nfeats = NumaLoadSVMExamples(scan, node_train_examps, nnodes);
  }
  if (matlab_tsv) {
    MatlabTSVFileScanner scantest(szTestFile);
    NumaLoadSVMExamples(scantest, node_test_examps, nnodes);
  } else {
    TSVFileScanner scantest(szTestFile);
    NumaLoadSVMExamples(scantest, node_test_examps, nnodes);
  }

  unsigned *degs = new unsigned[nfeats];
  printf("Loaded %lu examples\n", nfeats);
  for (size_t i = 0; i < nfeats; i++) {
    degs[i] = 0;
  }
  CountDegrees(node_train_examps[0], degs);
  tp.degrees = degs;
  tp.ndim = nfeats;

//  hogwild::freeforall::FeedTrainTest(memfeed.GetTrough(), nepochs, nthreads);
  NumaSVMModel * node_m;
  node_m = new NumaSVMModel[nnodes];
  CreateNumaSVMModel(node_m, nfeats, nnodes, nthreads); 
  NumaSVMModel m;
  m.AllocateModel(nfeats);
  NumaMemoryScan<SVMExample> mscan(node_train_examps, nnodes);
  Hogwild<NumaSVMModel, SVMParams, NumaSVMExec>  hw(node_m[0], tp, tpool);
  NumaMemoryScan<SVMExample> tscan(node_test_examps, nnodes);

  hw.RunExperiment(nepochs, wall_clock, mscan, tscan);

  return 0;
}


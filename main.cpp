//
//  main.cpp
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 25/02/2020.
//  Copyright © 2020 Vilhelm Sævarsson. All rights reserved.
//

#include <iostream>
#include <time.h>

#include "allocator.h"

const int NUMBER_OF_ITEMS = 1000;
const int NUMBER_OF_ITERATIONS = 1000000;

thread_local size_t Allocator::_thread_id = 0;
thread_local int32_t Allocator::_thread_idx = 0;
int main(int argc, const char *argv[]) {

  Allocator alloc;
  char *variables[NUMBER_OF_ITEMS];
  char *variables_large[NUMBER_OF_ITERATIONS];
  auto t1 = std::chrono::high_resolution_clock::now();
  for (int j = 0; j < NUMBER_OF_ITERATIONS; j++) {
    auto _t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUMBER_OF_ITEMS; i++)
      variables[i] = (char *)alloc.malloc(16);
    for (int i = 0; i < NUMBER_OF_ITEMS; i++)
      alloc.free(variables[i]);
  }
  auto t2 = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  std::cout << "Time spent in alloc: " << duration << " milliseconds"
            << std::endl
            << std::endl;
  t1 = std::chrono::high_resolution_clock::now();
  for (int j = 0; j < NUMBER_OF_ITERATIONS; j++) {
    for (int i = 0; i < NUMBER_OF_ITEMS; i++)
      variables[i] = (char *)malloc(16);

    for (int i = 0; i < NUMBER_OF_ITEMS; i++)
      free(variables[i]);
  }
  t2 = std::chrono::high_resolution_clock::now();
  duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  std::cout << "Time spent in mi_malloc: " << duration << " milliseconds"
            << std::endl
            << std::endl;

  /*
  for(int j=0;j<NUMBER_OF_ITERATIONS; j++)
  {
     for(int i=0; i<NUMBER_OF_ITEMS; i++)
         variables[i] = (char*)malloc(2);

      for(int i=0; i<NUMBER_OF_ITEMS; i++)
          free( variables[i] );
  }

  t2 = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1
  ).count(); std::cout << "Time spent in malloc: " << duration << "
  milliseconds" << std::endl << std::endl;


  int sizes[] = {2, 200, 8, 311, 16, 612, 32, 48};
  std::cout << "Starting to test the custom new and delete calls..." <<
  std::endl; t1 = std::chrono::high_resolution_clock::now(); int k = 0; for(int
  j=0;j<NUMBER_OF_ITERATIONS; j++)
  {

      for(int i=0; i<NUMBER_OF_ITEMS; i++)
      {
          k++;
          variables[i] = (char*)mm_malloc(sizes[k%8]);
      }

      for(int i=0; i<NUMBER_OF_ITEMS; i++)
          mm_free( variables[i] );

  }

  t2 = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1
  ).count(); std::cout << "Time spent using normal new/delete calls: " <<
  duration << " milliseconds" << std::endl << std::endl;

  char* variables2[NUMBER_OF_ITEMS];
  std::cout << "Starting to test the normal new and delete calls..." <<
  std::endl; k = 0; t1 = std::chrono::high_resolution_clock::now(); for(int
  j=0;j<NUMBER_OF_ITERATIONS; j++)
  {
      for(int i=0; i<NUMBER_OF_ITEMS; i++)
      {
          k++;
          variables2[i] = (char*)malloc(sizes[k%8]);
      }


      for(int i=0; i<NUMBER_OF_ITEMS; i++)
          free( variables2[i] );
  }

  t2 = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1
  ).count(); std::cout << "Time spent using normal new/delete calls: " <<
  duration << " milliseconds" << std::endl << std::endl;
   */
  /*
  std::cout << "Starting to test the mimalloc new and delete calls..." <<
  std::endl; k = 0; t1 = std::chrono::high_resolution_clock::now(); for(int
  j=0;j<NUMBER_OF_ITERATIONS; j++)
  {
     for(int i=0; i<NUMBER_OF_ITEMS; i++)
     {
         k++;
         variables[i] = (char*)mi_malloc(sizes[k%8]);
     }


     for(int i=0; i<NUMBER_OF_ITEMS; i++)
         mi_free( variables[i] );
  }

  t2 = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1
  ).count(); std::cout << "Time spent using normal new/delete calls: " <<
  duration << " milliseconds" << std::endl << std::endl;
   */
  return 0;
}

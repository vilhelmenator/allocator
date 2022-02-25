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
int main(int argc, const char *argv[])
{

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
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
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
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    std::cout << "Time spent in mi_malloc: " << duration << " milliseconds"
              << std::endl
              << std::endl;

    return 0;
}

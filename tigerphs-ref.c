/*
   TigerPHS reference C implementation

   Written in 2014 by Bill Cox <waywardgeek@gmail.com>

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along with
   this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hkdf/sha.h"
#include "tigerphs.h"
#include "tigerphs-impl.h"

// Add the last hashed data into the result.
static void addIntoHash(uint32_t *hash256, uint32_t *mem, uint32_t parallelism, uint32_t blocklen,
        uint32_t blocksPerThread) {
    for(uint32_t p = 0; p < parallelism; p++) {
        for(uint32_t i = 0; i < 8; i++) {
            hash256[i] += mem[(p+1)*(uint64_t)blocklen*blocksPerThread + i - 8];
        }
    }
}

// Compute the bit reversal of v.
static uint32_t reverse(uint32_t v, uint32_t numBits) {
    uint32_t result = 0;
    while(numBits-- != 0) {
        result = (result << 1) | (v & 1);
        v >>= 1;
    }
    return result;
}

// Hash three blocks together with fast SSE friendly hash function optimized for high memory bandwidth.
static void hashBlocks(uint32_t state[8], uint32_t *mem, uint32_t blocklen, uint32_t subBlocklen,
        uint64_t fromAddr, uint64_t prevAddr, uint64_t toAddr, uint32_t multiplies, uint32_t repetitions) {

    // Do SIMD friendly memory hashing and a scalar CPU friendly parallel multiplication chain
    uint32_t numSubBlocks = blocklen/subBlocklen;
    uint32_t oddState[8];
    for(uint32_t i = 0; i < 8; i++) {
        oddState[i] = state[i] | 1;
    }
    uint64_t v = 1;

    for(uint32_t r = 0; r < repetitions; r++) {
        uint32_t *f = mem + fromAddr;
        uint32_t *t = mem + toAddr;
        for(uint32_t i = 0; i < numSubBlocks; i++) {
            uint32_t randVal = *f;
            uint32_t *p = mem + prevAddr + subBlocklen*(randVal & (numSubBlocks - 1));
            for(uint32_t j = 0; j < subBlocklen/8; j++) {

                // Compute the multiplication chain
                for(uint32_t k = 0; k < multiplies; k++) {
                    v = (uint32_t)v * (uint64_t)oddState[k];
                    v ^= randVal;
                    randVal += v >> 32;
                }

                // Hash 32 bytes of memory
                for(uint32_t k = 0; k < 8; k++) {
                    state[k] = (state[k] + *p++) ^ *f++;
                    state[k] = (state[k] >> 24) | (state[k] << 8);
                    *t++ = state[k];
                }
            }
        }
    }
    hashWithSalt(state, state, v);
}

// Hash memory without doing any password dependent memory addressing to thwart cache-timing-attacks.
// Use Solar Designer's sliding-power-of-two window, with Catena's bit-reversal.
static void hashWithoutPassword(uint32_t *state, uint32_t *mem, uint32_t p, uint64_t blocklen,
        uint32_t blocksPerThread, uint32_t multiplies, uint32_t repetitions, uint32_t parallelism,
        uint32_t completedBlocks) {

    uint64_t start = blocklen*blocksPerThread*p;
    uint32_t firstBlock = completedBlocks;
    if(completedBlocks == 0) {
        // Initialize the first block of memory
        for(uint32_t i = 0; i < blocklen/8; i++) {
            hashWithSalt(mem + start + 8*i, state, i);
        }
        firstBlock = 1;
    }

    // Hash one "slice" worth of memory hashing
    uint32_t numBits = 1; // The number of bits in i
    for(uint32_t i = firstBlock; i < completedBlocks + blocksPerThread/TIGERPHS_SLICES; i++) {
        while(1 << numBits <= i) {
            numBits++;
        }

        // Compute the "sliding reverse" block position
        uint32_t reversePos = reverse(i, numBits-1);
        if(reversePos + (1 << (numBits-1)) < i) {
            reversePos += 1 << (numBits-1);
        }
        uint64_t fromAddr = blocklen*reversePos;

        // Compute which thread's memory to read from
        if(fromAddr < completedBlocks*blocklen) {
            fromAddr += blocklen*blocksPerThread*(i % parallelism);
        } else {
            fromAddr += start;
        }

        uint64_t toAddr = start + i*blocklen;
        uint64_t prevAddr = toAddr - blocklen;
        hashBlocks(state, mem, blocklen, blocklen, fromAddr, prevAddr, toAddr, multiplies, repetitions);
    }
}

// Hash memory with password dependent addressing.
static void hashWithPassword(uint32_t *state, uint32_t *mem, uint32_t p, uint64_t blocklen,
        uint32_t blocksPerThread, uint32_t multiplies, uint32_t repetitions, uint32_t parallelism,
        uint32_t completedBlocks) {

    uint64_t start = blocklen*blocksPerThread*p;

    // Hash one "slice" worth of memory hashing
    for(uint32_t i = completedBlocks; i < completedBlocks + blocksPerThread/TIGERPHS_SLICES; i++) {

        // Compute rand()^3 distance distribution
        uint64_t v = state[0];
        uint64_t v2 = v*v >> 32;
        uint64_t v3 = v*v2 >> 32;
        uint32_t distance = (i-1)*v3 >> 32;

        // Hash the prior block and the block at 'distance' blocks in the past
        uint64_t fromAddr = (i - 1 - distance)*blocklen;

        // Compute which thread's memory to read from
        if(fromAddr < completedBlocks*blocklen) {
            fromAddr += blocklen*(state[1] % parallelism)*blocksPerThread;
        } else {
            fromAddr += start;
        }

        uint64_t toAddr = start + i*blocklen;
        uint64_t prevAddr = toAddr - blocklen;
        hashBlocks(state, mem, blocklen, TIGERPHS_SUBBLOCKLEN, fromAddr, prevAddr, toAddr, multiplies, repetitions);
    }
}

// Hash memory for one level of garlic.
static void hashMemory(uint8_t *hash, uint8_t hashSize, uint32_t *mem, uint8_t memCost, uint8_t timeCost,
        uint8_t parallelism, uint32_t resistantSlices) {

    uint32_t blocklen, blocksPerThread, repetitions;
    uint8_t multiplies;

    // Determine parameters that meet the memory goal
    TigerPHS_ComputeSizes(memCost, timeCost, &parallelism, &blocklen, &blocksPerThread, &repetitions, &multiplies);

    // Convert hash to 8 32-bit ints.
    uint32_t hash256[8];
    TigerPHS_hkdfExtract(hash256, hash, hashSize);
    secureZeroMemory(hash, hashSize);

    // Initialize thread states
    uint32_t states[8*parallelism];
    for(uint32_t p = 0; p < parallelism; p++) {
        hashWithSalt(states + 8*p, hash256, p);
    }

    for(uint32_t slice = 0; slice < TIGERPHS_SLICES; slice++) {
        for(uint32_t p = 0; p < parallelism; p++) {
            if(slice < resistantSlices) {
                hashWithoutPassword(states + 8*p, mem, p, blocklen, blocksPerThread, multiplies, repetitions,
                    parallelism, slice*blocksPerThread/TIGERPHS_SLICES);
            } else {
                hashWithPassword(states + 8*p, mem, p, blocklen, blocksPerThread, multiplies, repetitions,
                    parallelism, slice*blocksPerThread/TIGERPHS_SLICES);
            }
        }
    }

    // Apply a crypto-strength hash
    addIntoHash(hash256, mem, parallelism, blocklen, blocksPerThread);
    TigerPHS_hkdfExpand(hash, hashSize, hash256);
}

// The TigerPHS password hashing function.  Return false if there is a memory allocation error.
bool TigerPHS(uint8_t *hash, uint32_t hashSize, uint8_t startMemCost, uint8_t stopMemCost, uint8_t timeCost,
        uint8_t multiplies, uint8_t parallelism, bool updateMemCostMode) {

    // Allocate memory
    uint32_t *mem = malloc((uint64_t)1024 << stopMemCost);
    if(mem == NULL) {
        fprintf(stderr, "Unable to allocate memory\n");
        return false;
    }

    // Iterate through the levels of garlic.  Throw away some early memory to reduce the
    // danger from leaking memory to an attacker.
    for(uint8_t i = 0; i <= stopMemCost; i++) {
        if(i >= startMemCost || (!updateMemCostMode && i + 6 < startMemCost)) {
            uint32_t resistantSlices = TIGERPHS_SLICES/2;
            if(i < startMemCost) {
                resistantSlices = TIGERPHS_SLICES;
            }
            hashMemory(hash, hashSize, mem, i, timeCost, parallelism, resistantSlices);
            if(i != stopMemCost) {
                // Not doing the last hash is for server relief support
                TigerPHS_hkdf(hash, hashSize);
            }
        }
    }

    // The light is green, the trap is clean
    //dumpMemory("dieharder_data", mem, ((uint64_t)1024 << stopMemCost)/4);
    free(mem);
    return true;
}

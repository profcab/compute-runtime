/*
 * Copyright (c) 2018, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/command_stream/experimental_command_buffer.h"
#include "runtime/command_stream/linear_stream.h"
#include "runtime/memory_manager/memory_constants.h"
#include "runtime/memory_manager/memory_manager.h"
#include <cstring>
#include <type_traits>

namespace OCLRT {

ExperimentalCommandBuffer::ExperimentalCommandBuffer(CommandStreamReceiver *csr, double profilingTimerResolution) : commandStreamReceiver(csr),
                                                                                                                    currentStream(nullptr),
                                                                                                                    timestampsOffset(0),
                                                                                                                    experimentalAllocationOffset(0),
                                                                                                                    defaultPrint(true),
                                                                                                                    timerResolution(profilingTimerResolution) {
    timestamps = csr->getMemoryManager()->allocateGraphicsMemory(MemoryConstants::pageSize);
    memset(timestamps->getUnderlyingBuffer(), 0, timestamps->getUnderlyingBufferSize());
    experimentalAllocation = csr->getMemoryManager()->allocateGraphicsMemory(MemoryConstants::pageSize);
    memset(experimentalAllocation->getUnderlyingBuffer(), 0, experimentalAllocation->getUnderlyingBufferSize());
}

ExperimentalCommandBuffer::~ExperimentalCommandBuffer() {
    auto timestamp = static_cast<uint64_t *>(timestamps->getUnderlyingBuffer());
    for (uint32_t i = 0; i < timestampsOffset / (2 * sizeof(uint64_t)); i++) {
        auto stop = static_cast<uint64_t>(*(timestamp + 1) * timerResolution);
        auto start = static_cast<uint64_t>(*timestamp * timerResolution);
        auto delta = stop - start;
        printDebugString(defaultPrint, stdout, "#%u: delta %llu start %llu stop %llu\n", i, delta, start, stop);
        timestamp += 2;
    }
    MemoryManager *memManager = commandStreamReceiver->getMemoryManager();
    if (memManager) {
        memManager->freeGraphicsMemory(timestamps);
        memManager->freeGraphicsMemory(experimentalAllocation);

        if (currentStream.get()) {
            memManager->storeAllocation(std::unique_ptr<GraphicsAllocation>(currentStream->getGraphicsAllocation()), REUSABLE_ALLOCATION);
            currentStream->replaceGraphicsAllocation(nullptr);
        }
    }
}

void ExperimentalCommandBuffer::getCS(size_t minRequiredSize) {
    if (!currentStream) {
        currentStream.reset(new LinearStream(nullptr));
    }
    minRequiredSize += CSRequirements::minCommandQueueCommandStreamSize;
    if (currentStream->getAvailableSpace() < minRequiredSize) {
        MemoryManager *memManager = commandStreamReceiver->getMemoryManager();
        // If not, allocate a new block. allocate full pages
        minRequiredSize = alignUp(minRequiredSize, MemoryConstants::pageSize);

        auto requiredSize = minRequiredSize + CSRequirements::csOverfetchSize;

        GraphicsAllocation *allocation = memManager->obtainReusableAllocation(requiredSize, false).release();
        if (!allocation) {
            allocation = memManager->allocateGraphicsMemory(requiredSize);
        }
        allocation->setAllocationType(GraphicsAllocation::AllocationType::LINEAR_STREAM);
        // Deallocate the old block, if not null
        auto oldAllocation = currentStream->getGraphicsAllocation();
        if (oldAllocation) {
            memManager->storeAllocation(std::unique_ptr<GraphicsAllocation>(oldAllocation), REUSABLE_ALLOCATION);
        }
        currentStream->replaceBuffer(allocation->getUnderlyingBuffer(), minRequiredSize - CSRequirements::minCommandQueueCommandStreamSize);
        currentStream->replaceGraphicsAllocation(allocation);
    }
}

void ExperimentalCommandBuffer::makeResidentAllocations() {
    commandStreamReceiver->makeResident(*currentStream->getGraphicsAllocation());
    commandStreamReceiver->makeResident(*timestamps);
    commandStreamReceiver->makeResident(*experimentalAllocation);
}

} // namespace OCLRT
